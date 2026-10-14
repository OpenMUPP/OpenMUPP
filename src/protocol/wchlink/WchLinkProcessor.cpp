#include "protocol/wchlink/WchLinkProcessor.h"

#include <algorithm>

namespace wchlink {

namespace {

namespace dm = rvswd::dm;

// The fields of a request payload. A read past the end gives 0, as the
// WCH-LinkE does not check the length.
class PayloadReader {
   public:
   explicit PayloadReader(art::span<const uint8_t> data)
      : data_(data) {
   }

   uint8_t u8() {
      return pos_ < data_.size() ? data_[pos_++] : 0;
   }

   // Big-endian
   uint32_t u32() {
      uint32_t value = 0;
      for(int i = 0; i < 4; ++i) {
         value = value << 8 | u8();
      }
      return value;
   }

   private:
   art::span<const uint8_t> data_;
   std::size_t pos_{ 0 };
};

// Most significant byte first
void putU32(uint8_t *out, uint32_t value) {
   out[0] = static_cast<uint8_t>(value >> 24);
   out[1] = static_cast<uint8_t>(value >> 16);
   out[2] = static_cast<uint8_t>(value >> 8);
   out[3] = static_cast<uint8_t>(value);
}

// The sub-commands of command 0x0d with a fixed reply
inline constexpr uint8_t controlUnknown3 = 0x03;
inline constexpr uint8_t controlUnknown4 = 0x04;
inline constexpr uint8_t controlUnknown4Reply = 0x00;

// The reset sub-command of command 0x0b
inline constexpr uint8_t resetAndRun = 0x01;

// dmcontrol: acknowledge havereset
inline constexpr uint32_t ackhavereset = 1u << 28;

} // namespace

Processor::Processor(Target &target, Platform &platform)
   : target_(target)
   , platform_(platform) {
}

void Processor::submit(art::span<const uint8_t> request) {
   requestLength_ = std::min(request.size(), request_.size());
   std::copy_n(request.begin(), requestLength_, request_.begin());
   hasRequest_ = true;
}

bool Processor::runNext() {
   if(!hasRequest_) {
      return false;
   }
   hasRequest_ = false;
   execute({ request_.data(), requestLength_ });
   return true;
}

void Processor::execute(art::span<const uint8_t> request) {
   if(request.size() < 3 || request[0] != requestHeader) {
      replyUnknown();
      return;
   }
   const uint8_t cmd = request[1];
   const auto payload = request.subspan(3, std::min<std::size_t>(request[2], request.size() - 3));
   PayloadReader in(payload);
   switch(cmd) {
   case command::control:
      controlCommand(in.u8());
      break;
   case command::configure: {
      const uint8_t chipType = in.u8();
      configure(chipType, in.u8());
      break;
   }
   case command::dmi:
      dmiCommand(payload);
      break;
   case command::reset:
      resetCommand(in.u8());
      break;
   case command::chipInfo:
      chipInfoCommand();
      break;
   case command::readMemory: {
      const uint32_t address = in.u32();
      setReadRange(address, in.u32());
      break;
   }
   case command::program:
      programCommand(in.u8());
      break;
   case command::disableDebug: {
      const uint8_t sub = in.u8();
      quit();
      replyByte(cmd, sub);
      break;
   }
   default:
      replyUnknown();
      break;
   }
}

void Processor::reply(uint8_t command, art::span<const uint8_t> payload) {
   response_[0] = responseHeader;
   response_[1] = command;
   response_[2] = static_cast<uint8_t>(payload.size());
   std::copy(payload.begin(), payload.end(), response_.begin() + 3);
   responseLength_ = 3 + payload.size();
}

void Processor::replyByte(uint8_t command, uint8_t value) {
   reply(command, { &value, 1 });
}

void Processor::replyError() {
   response_[0] = errorHeader;
   response_[1] = errorNoTarget;
   response_[2] = 1;
   response_[3] = 1;
   responseLength_ = 4;
}

void Processor::replyUnknown() {
   replyByte(requestHeader, 0xff);
}

void Processor::controlCommand(uint8_t sub) {
   switch(sub) {
   case control::info: {
      const std::array<uint8_t, 4> info{ firmwareMajor, firmwareMinor, probeType, 0 };
      reply(command::control, info);
      return;
   }
   case control::connect: {
      if(!connect()) {
         replyError();
         return;
      }
      std::array<uint8_t, 5> answer{ familyOf(wire_, chipId_) };
      putU32(&answer[1], chipId_);
      reply(command::control, answer);
      return;
   }
   case control::quit:
      quit();
      replyByte(command::control, sub);
      return;
   case controlUnknown4:
      replyByte(command::control, controlUnknown4Reply);
      return;
   case controlUnknown3:
   default:
      replyByte(command::control, sub);
      return;
   }
}

bool Processor::connect() {
   dataRemaining_ = 0;
   detach();
   const bool oneWireOnly = chipType_ == familyV00x;
   for(const auto wire : { rvswd::Wire::Two, rvswd::Wire::One }) {
      if(oneWireOnly && wire == rvswd::Wire::Two) {
         continue;
      }
      uint32_t chipId = 0;
      target_.attach(wire);
      if(connectOn(chipId)) {
         connected_ = true;
         wire_ = wire;
         chipId_ = chipId;
         return true;
      }
      target_.detach();
   }
   return false;
}

bool Processor::connectOn(uint32_t &chipId) {
   uint32_t status = 0;
   if(target_.dmiWrite(wchdm::unlock1, wchdm::unlockValue) != rvswd::Status::Ok ||
      target_.dmiWrite(wchdm::unlock2, wchdm::unlockValue) != rvswd::Status::Ok ||
      target_.readDmstatus(status) != rvswd::Status::Ok || target_.halt() != rvswd::Status::Ok ||
      target_.dmiRead(dm::chipId, chipId) != rvswd::Status::Ok) {
      return false;
   }
   // One wire: a frame without a target reads ffffffff.
   if(chipId == 0 || chipId == 0xffff'ffffu) {
      return false;
   }
   return target_.writeRegister(wchdm::csr7c0, wchdm::csr7c0Value) == rvswd::Status::Ok;
}

void Processor::quit() {
   dataRemaining_ = 0;
   if(!connected_) {
      return;
   }
   // The quit of the probe: resumereq, dmstatus, then dmactive 0. The
   // pins are released also when the hart does not resume.
   static_cast<void>(target_.resume());
   static_cast<void>(target_.dmiWrite(dm::dmcontrol, dm::resumereq));
   detach();
}

void Processor::detach() {
   if(connected_) {
      target_.detach();
      connected_ = false;
   }
}

void Processor::configure(uint8_t chipType, uint8_t speed) {
   chipType_ = chipType;
   if(speed >= static_cast<uint8_t>(Speed::High) && speed <= static_cast<uint8_t>(Speed::Low)) {
      target_.setSpeed(static_cast<Speed>(speed));
   }
   replyByte(command::configure, 1);
}

void Processor::dmiCommand(art::span<const uint8_t> payload) {
   PayloadReader in(payload);
   const uint8_t address = in.u8();
   uint32_t data = in.u32();
   const uint8_t op = in.u8();
   uint8_t status = dmiFailed;
   if(op == dmiNop) {
      status = dmiOk;
   } else if(connected_ && op == dmiRead) {
      status = target_.dmiRead(address, data) == rvswd::Status::Ok ? dmiOk : dmiFailed;
   } else if(connected_ && op == dmiWrite) {
      status = target_.dmiWrite(address, data) == rvswd::Status::Ok ? dmiOk : dmiFailed;
   }
   std::array<uint8_t, 6> answer{ address };
   putU32(&answer[1], data);
   answer[5] = status;
   reply(command::dmi, answer);
}

void Processor::resetCommand(uint8_t sub) {
   if(sub != resetAndRun) {
      replyUnknown();
      return;
   }
   // ndmreset without haltreq: the hart restarts and runs. Then the
   // havereset state is acknowledged.
   if(!connected_ || target_.dmiWrite(dm::dmcontrol, dm::ndmreset | dm::dmactive) != rvswd::Status::Ok) {
      replyError();
      return;
   }
   platform_.delayUs(resetPulseUs);
   if(target_.dmiWrite(dm::dmcontrol, dm::dmactive) != rvswd::Status::Ok ||
      target_.dmiWrite(dm::dmcontrol, ackhavereset | dm::dmactive) != rvswd::Status::Ok) {
      replyError();
      return;
   }
   replyByte(command::reset, sub);
}

void Processor::chipInfoCommand() {
   if(!connected_) {
      replyError();
      return;
   }
   for(std::size_t i = 0; i < chipInfoAddresses.size(); ++i) {
      uint32_t value = 0;
      if(target_.readMemory(chipInfoAddresses[i], value) != rvswd::Status::Ok) {
         replyError();
         return;
      }
      putU32(&response_[4 * i], value);
   }
   // Raw bytes: no header and no length
   putU32(&response_[16], chipId_);
   responseLength_ = chipInfoSize;
}

void Processor::setReadRange(uint32_t address, uint32_t length) {
   dataRemaining_ = 0;
   readAddress_ = address;
   readLength_ = length;
   replyByte(command::readMemory, 1);
}

void Processor::programCommand(uint8_t sub) {
   if(sub != programReadMemory) {
      // The flash sub-commands (W5)
      replyUnknown();
      return;
   }
   if(!connected_) {
      replyError();
      return;
   }
   dataAddress_ = readAddress_;
   dataRemaining_ = readLength_;
   dataFailed_ = false;
   replyByte(command::program, sub);
}

art::span<const uint8_t> Processor::dataPacket() {
   const std::size_t count = std::min<std::size_t>(dataRemaining_, data_.size());
   for(std::size_t offset = 0; offset < count; offset += 4) {
      uint32_t value = 0xffff'ffffu;
      if(!dataFailed_ && (!connected_ || target_.readMemory(dataAddress_, value) != rvswd::Status::Ok)) {
         // No more target accesses: a missing target would make each word
         // wait for a timeout.
         dataFailed_ = true;
         value = 0xffff'ffffu;
      }
      putU32(&data_[offset], value);
      dataAddress_ += 4;
   }
   dataRemaining_ -= static_cast<uint32_t>(count);
   return { data_.data(), count };
}

void Processor::hostGone() {
   dataRemaining_ = 0;
   hasRequest_ = false;
   responseLength_ = 0;
   detach();
}

void Processor::release() {
   dataRemaining_ = 0;
   detach();
}

} // namespace wchlink
