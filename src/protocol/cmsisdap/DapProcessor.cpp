#include "protocol/cmsisdap/DapProcessor.h"

#include <algorithm>
#include <cstring>

namespace cmsisdap {

// Reads the fields of a request. A read past the end gives 0 and clears ok().
class PacketReader {
   public:
   explicit PacketReader(art::span<const uint8_t> data)
      : data_(data) {
   }

   uint8_t u8() {
      if(pos_ >= data_.size()) {
         ok_ = false;
         return 0;
      }
      return data_[pos_++];
   }

   uint16_t u16() {
      const uint16_t low = u8();
      return static_cast<uint16_t>(low | u8() << 8);
   }

   uint32_t u32() {
      const uint32_t low = u16();
      return low | static_cast<uint32_t>(u16()) << 16;
   }

   art::span<const uint8_t> bytes(std::size_t count) {
      if(count > data_.size() - pos_) {
         ok_ = false;
         pos_ = data_.size();
         return {};
      }
      const auto result = data_.subspan(pos_, count);
      pos_ += count;
      return result;
   }

   void skip(std::size_t count) {
      static_cast<void>(bytes(count));
   }

   [[nodiscard]] std::size_t remaining() const {
      return data_.size() - pos_;
   }

   [[nodiscard]] bool ok() const {
      return ok_;
   }

   private:
   art::span<const uint8_t> data_;
   std::size_t pos_{ 0 };
   bool ok_{ true };
};

// Writes a response. A write past the end is lost and clears ok().
class PacketWriter {
   public:
   explicit PacketWriter(art::span<uint8_t> data)
      : data_(data) {
   }

   void u8(uint8_t value) {
      if(size_ >= data_.size()) {
         ok_ = false;
         return;
      }
      data_[size_++] = value;
   }

   void u16(uint16_t value) {
      u8(static_cast<uint8_t>(value));
      u8(static_cast<uint8_t>(value >> 8));
   }

   void u32(uint32_t value) {
      u16(static_cast<uint16_t>(value));
      u16(static_cast<uint16_t>(value >> 16));
   }

   void bytes(art::span<const uint8_t> value) {
      for(const uint8_t byte : value) {
         u8(byte);
      }
   }

   // Space for count bytes (0), filled in later with set().
   std::size_t reserve(std::size_t count) {
      const std::size_t position = size_;
      for(std::size_t i = 0; i < count; ++i) {
         u8(0);
      }
      return position;
   }

   void set(std::size_t position, uint8_t value) {
      if(position < size_) {
         data_[position] = value;
      }
   }

   // Cuts the response to size and clears the overflow.
   void truncate(std::size_t size) {
      size_ = std::min(size_, size);
      ok_ = true;
   }

   [[nodiscard]] std::size_t size() const {
      return size_;
   }

   [[nodiscard]] std::size_t room() const {
      return data_.size() - size_;
   }

   [[nodiscard]] bool ok() const {
      return ok_;
   }

   private:
   art::span<uint8_t> data_;
   std::size_t size_{ 0 };
   bool ok_{ true };
};

namespace {

// The status of the commands
constexpr uint8_t statusOk = 0;
constexpr uint8_t statusError = 1;

// The ports of DAP_Connect
constexpr uint8_t portDefault = 0;
constexpr uint8_t portSwd = 1;

// The pins of DAP_SWJ_Pins, and the GPIO bit of each. nTRST and nRESET are
// open-drain: a 1 releases the pin.
struct SwjPin {
   uint8_t bit;
   uint16_t gpio;
   protocol::PinMode mode;
   bool output;
};

constexpr std::array<SwjPin, 6> swjPinTable{ {
{ 0x01, protocol::gpio::tck, protocol::PinMode::PushPull, true },
{ 0x02, protocol::gpio::tms, protocol::PinMode::PushPull, true },
{ 0x04, protocol::gpio::tdi, protocol::PinMode::PushPull, true },
{ 0x08, protocol::gpio::tdo, protocol::PinMode::Input, false },
{ 0x20, protocol::gpio::trst, protocol::PinMode::OpenDrain, true },
{ 0x80, protocol::gpio::srst, protocol::PinMode::OpenDrain, true },
} };

// DAP_HostStatus
constexpr uint8_t hostStatusConnected = 0;
constexpr uint8_t hostStatusRunning = 1;

constexpr char protocolVersion[] = "2.1.0";

} // namespace

Processor::Processor(DapSwd &swd, protocol::GpioPins &gpio, Platform &platform, const DeviceInfo &info)
   : swd_(swd)
   , gpio_(gpio)
   , platform_(platform)
   , info_(info) {
   swd_.apply(settings_);
   swd_.setClockHz(clockHz_);
}

art::span<const uint8_t> Processor::response() const {
   if(responses_ == 0) {
      return {};
   }
   return { responseBuffer_[responseFirst_].data(), responseLength_[responseFirst_] };
}

void Processor::popResponse() {
   if(responses_ != 0) {
      responseFirst_ = (responseFirst_ + 1) % packetCount;
      --responses_;
   }
}

void Processor::hostGone() {
   requests_ = 0;
   requestFirst_ = 0;
   abortRequests_ = 0;
   // The responses are for the old host.
   responses_ = 0;
   responseFirst_ = 0;
   disconnect();
}

void Processor::submit(art::span<const uint8_t> request) {
   if(request.empty()) {
      return;
   }
   if(request[0] == command::transferAbort) {
      // All packets that came before it, also the one that runs
      abortRequests_ = requests_;
      return;
   }
   if(!canSubmit()) {
      return;
   }
   const std::size_t index = (requestFirst_ + requests_) % packetCount;
   const std::size_t length = std::min(request.size(), requestBuffer_[index].size());
   std::copy_n(request.begin(), length, requestBuffer_[index].begin());
   requestLength_[index] = static_cast<uint8_t>(length);
   ++requests_;
}

void Processor::run() {
   if(running_) {
      return;
   }
   running_ = true;
   while(requests_ != 0 && !mustWait()) {
      const auto front = request(0);
      if(isQueued(0)) {
         front[0] = command::executeCommands;
      }
      // submit() can add packets behind this one while it runs.
      execute(front);
      requestFirst_ = (requestFirst_ + 1) % packetCount;
      --requests_;
      if(abortRequests_ != 0) {
         --abortRequests_;
      }
   }
   running_ = false;
}

art::span<uint8_t> Processor::request(std::size_t index) {
   const std::size_t slot = (requestFirst_ + index) % packetCount;
   return { requestBuffer_[slot].data(), requestLength_[slot] };
}

bool Processor::isQueued(std::size_t index) {
   const auto packet = request(index);
   return packet[0] == command::queueCommands && packet.size() <= packetSize;
}

// The DAP_QueueCommands packets wait for the next packet with another
// command. A queued packet in the last free slot runs the queue: no
// response could free a slot for the packet that ends the queue.
bool Processor::mustWait() {
   if(!canSubmit()) {
      return false;
   }
   for(std::size_t i = 0; i < requests_; ++i) {
      if(!isQueued(i)) {
         return false;
      }
   }
   return true;
}

// Runs a packet and appends its response.
void Processor::execute(art::span<const uint8_t> request) {
   const std::size_t slot = (responseFirst_ + responses_) % packetCount;
   PacketWriter out({ responseBuffer_[slot].data(), packetSize });
   if(request.size() > packetSize) {
      out.u8(command::invalid);
   } else {
      PacketReader in(request);
      if(!processCommand(in, out, false) || !out.ok()) {
         out.truncate(0);
         out.u8(command::invalid);
      }
   }
   responseLength_[slot] = static_cast<uint8_t>(out.size());
   ++responses_;
}

bool Processor::processCommand(PacketReader &in, PacketWriter &out, bool nested) {
   const uint8_t id = in.u8();
   if(!in.ok()) {
      return false;
   }
   switch(id) {
   case command::info:
      return infoCommand(in, out);
   case command::hostStatus:
      return hostStatus(in, out);
   case command::connect:
      return connect(in, out);
   case command::disconnect:
      disconnect();
      out.u8(id);
      out.u8(statusOk);
      return true;
   case command::transferConfigure:
      return transferConfigure(in, out);
   case command::transfer:
      return transfer(in, out);
   case command::transferBlock:
      return transferBlock(in, out);
   case command::writeAbort:
      return writeAbort(in, out);
   case command::delay:
      return delay(in, out);
   case command::resetTarget:
      resetTarget(out);
      return true;
   case command::swjPins:
      return swjPins(in, out);
   case command::swjClock:
      return swjClock(in, out);
   case command::swjSequence:
      return swjSequence(in, out);
   case command::swdConfigure:
      return swdConfigure(in, out);
   case command::swdSequence:
      return swdSequence(in, out);
   case command::executeCommands:
   case command::queueCommands:
      if(!nested) {
         return executeCommands(id, in, out);
      }
      [[fallthrough]];
   default:
      out.u8(command::invalid);
      return true;
   }
}

bool Processor::executeCommands(uint8_t id, PacketReader &in, PacketWriter &out) {
   const uint8_t count = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(id);
   const std::size_t countPosition = out.reserve(1);
   uint8_t executed = 0;
   for(uint8_t i = 0; i < count; ++i) {
      const std::size_t mark = out.size();
      if(!processCommand(in, out, true) || !out.ok()) {
         // Cut the command that did not fit.
         out.truncate(mark);
         break;
      }
      ++executed;
   }
   out.set(countPosition, executed);
   return true;
}

bool Processor::infoCommand(PacketReader &in, PacketWriter &out) {
   const uint8_t id = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::info);
   const auto text = [&out](const char *value) {
      const std::size_t length = value == nullptr ? 0 : std::min(std::strlen(value), packetSize - 3);
      if(length == 0) {
         out.u8(0);
         return;
      }
      out.u8(static_cast<uint8_t>(length + 1));
      out.bytes({ reinterpret_cast<const uint8_t *>(value), length });
      out.u8(0);
   };
   switch(id) {
   case infoId::vendor:
      text(info_.vendor);
      break;
   case infoId::product:
      text(info_.product);
      break;
   case infoId::serialNumber:
      text(info_.serialNumber);
      break;
   case infoId::protocolVersion:
      text(protocolVersion);
      break;
   case infoId::firmwareVersion:
      text(info_.firmwareVersion);
      break;
   case infoId::capabilities:
      out.u8(info_.capabilities > 0xFF ? 2 : 1);
      out.u8(static_cast<uint8_t>(info_.capabilities));
      if(info_.capabilities > 0xFF) {
         out.u8(static_cast<uint8_t>(info_.capabilities >> 8));
      }
      break;
   case infoId::packetCount:
      out.u8(1);
      out.u8(static_cast<uint8_t>(packetCount));
      break;
   case infoId::packetSize:
      out.u8(2);
      out.u16(static_cast<uint16_t>(packetSize));
      break;
   default:
      // Not available: the target device and board, the test domain timer,
      // and the UART and SWO buffers
      out.u8(0);
      break;
   }
   return true;
}

bool Processor::hostStatus(PacketReader &in, PacketWriter &out) {
   const uint8_t type = in.u8();
   const uint8_t status = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::hostStatus);
   if(type == hostStatusConnected) {
      platform_.setLed(Led::Connected, (status & 1u) != 0);
   } else if(type == hostStatusRunning) {
      platform_.setLed(Led::Running, (status & 1u) != 0);
   } else {
      out.u8(statusError);
      return true;
   }
   out.u8(statusOk);
   return true;
}

bool Processor::connect(PacketReader &in, PacketWriter &out) {
   const uint8_t port = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::connect);
   if(port != portDefault && port != portSwd) {
      // JTAG is not available yet.
      out.u8(0);
      return true;
   }
   if(connected_) {
      swd_.fini();
   }
   // Another user of the engine may have changed it.
   swd_.apply(settings_);
   swd_.setClockHz(clockHz_);
   if((gpio_.available() & protocol::gpio::tdo) != 0) {
      gpio_.configure(protocol::gpio::tdo, protocol::PinMode::Input, false);
   }
   swd_.init();
   connected_ = true;
   out.u8(portSwd);
   return true;
}

void Processor::disconnect() {
   if(connected_) {
      swd_.fini();
      connected_ = false;
   } else {
      // DAP_SWJ_Pins can drive SWCLK and SWDIO without a connection.
      for(const uint16_t bit : { protocol::gpio::tck, protocol::gpio::tms }) {
         if((gpio_.available() & bit) != 0) {
            gpio_.configure(bit, protocol::PinMode::Input, false);
         }
      }
   }
   // Release the pins that DAP_SWJ_Pins drove, also without a connection:
   // a target must not stay in reset when the host stops. SRST and TRST
   // get the pull-up, as in the GPIO block.
   const uint16_t available = gpio_.available();
   for(const uint16_t bit : { protocol::gpio::srst, protocol::gpio::trst }) {
      if((available & bit) != 0) {
         gpio_.configure(bit, protocol::PinMode::InputPullUp, true);
      }
   }
   if((available & protocol::gpio::tdi) != 0) {
      gpio_.configure(protocol::gpio::tdi, protocol::PinMode::Input, false);
   }
}

bool Processor::transferConfigure(PacketReader &in, PacketWriter &out) {
   settings_.idleCycles = in.u8();
   settings_.waitRetries = in.u16();
   matchRetries_ = in.u16();
   if(!in.ok()) {
      return false;
   }
   swd_.apply(settings_);
   out.u8(command::transferConfigure);
   out.u8(statusOk);
   return true;
}

bool Processor::swdConfigure(PacketReader &in, PacketWriter &out) {
   const uint8_t configuration = in.u8();
   if(!in.ok()) {
      return false;
   }
   settings_.turnaround = static_cast<uint8_t>((configuration & 0x03u) + 1);
   settings_.dataPhase = (configuration & 0x04u) != 0;
   swd_.apply(settings_);
   out.u8(command::swdConfigure);
   out.u8(statusOk);
   return true;
}

bool Processor::swjClock(PacketReader &in, PacketWriter &out) {
   const uint32_t hz = in.u32();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::swjClock);
   if(hz == 0) {
      out.u8(statusError);
      return true;
   }
   clockHz_ = hz;
   swd_.setClockHz(hz);
   out.u8(statusOk);
   return true;
}

bool Processor::swjSequence(PacketReader &in, PacketWriter &out) {
   // A count of 0 is 256 bits.
   const uint8_t count = in.u8();
   const std::size_t bits = count == 0 ? 256 : count;
   const auto data = in.bytes((bits + 7) / 8);
   if(!in.ok()) {
      return false;
   }
   out.u8(command::swjSequence);
   if(!connected_) {
      out.u8(statusError);
      return true;
   }
   swd_.sequenceOut(static_cast<uint16_t>(bits), data);
   out.u8(statusOk);
   return true;
}

bool Processor::swdSequence(PacketReader &in, PacketWriter &out) {
   const uint8_t count = in.u8();
   if(!in.ok()) {
      return false;
   }
   // Check the whole request and the room for the data before the first
   // sequence.
   {
      PacketReader check = in;
      std::size_t inputBytes = 0;
      for(uint8_t i = 0; i < count; ++i) {
         const uint8_t info = check.u8();
         const std::size_t bytes = ((((info & 0x3Fu) == 0) ? 64 : (info & 0x3Fu)) + 7) / 8;
         if((info & 0x80u) != 0) {
            inputBytes += bytes;
         } else {
            check.skip(bytes);
         }
      }
      if(!check.ok() || inputBytes + 2 > out.room()) {
         return false;
      }
   }
   out.u8(command::swdSequence);
   const std::size_t statusPosition = out.reserve(1);
   if(!connected_) {
      out.set(statusPosition, statusError);
   }
   for(uint8_t i = 0; i < count; ++i) {
      // A cycle count of 0 is 64 cycles. Bit 7: read SWDIO.
      const uint8_t info = in.u8();
      const std::size_t bits = (info & 0x3Fu) == 0 ? 64 : (info & 0x3Fu);
      const std::size_t bytes = (bits + 7) / 8;
      if((info & 0x80u) != 0) {
         if(connected_) {
            std::array<uint8_t, 8> data{};
            swd_.sequenceIn(static_cast<uint16_t>(bits), { data.data(), bytes });
            out.bytes({ data.data(), bytes });
         }
      } else {
         const auto data = in.bytes(bytes);
         if(connected_ && in.ok()) {
            swd_.sequenceOut(static_cast<uint16_t>(bits), data);
         }
      }
      if(!in.ok()) {
         return false;
      }
   }
   return true;
}

bool Processor::delay(PacketReader &in, PacketWriter &out) {
   const uint16_t microseconds = in.u16();
   if(!in.ok()) {
      return false;
   }
   platform_.delayUs(microseconds);
   out.u8(command::delay);
   out.u8(statusOk);
   return true;
}

void Processor::resetTarget(PacketWriter &out) {
   out.u8(command::resetTarget);
   // Execute: 1 when the unit did the reset. Without an SRST pin, the
   // host has to reset the target itself.
   if((gpio_.available() & protocol::gpio::srst) == 0) {
      out.u8(statusOk);
      out.u8(0);
      return;
   }
   gpio_.configure(protocol::gpio::srst, protocol::PinMode::OpenDrain, false);
   platform_.delayUs(resetPulseUs);
   gpio_.configure(protocol::gpio::srst, protocol::PinMode::OpenDrain, true);
   out.u8(statusOk);
   out.u8(1);
}

uint8_t Processor::pinsIn() const {
   uint16_t mask = 0;
   for(const SwjPin &pin : swjPinTable) {
      mask = static_cast<uint16_t>(mask | pin.gpio);
   }
   const uint16_t value = gpio_.read(mask & gpio_.available());
   uint8_t result = 0;
   for(const SwjPin &pin : swjPinTable) {
      if((value & pin.gpio) != 0) {
         result = static_cast<uint8_t>(result | pin.bit);
      }
   }
   return result;
}

bool Processor::swjPins(PacketReader &in, PacketWriter &out) {
   const uint8_t output = in.u8();
   const uint8_t select = in.u8();
   const uint32_t wait = std::min(in.u32(), maxPinWaitUs);
   if(!in.ok()) {
      return false;
   }
   uint8_t selected = 0;
   for(const SwjPin &pin : swjPinTable) {
      if(pin.output && (select & pin.bit) != 0 && (gpio_.available() & pin.gpio) != 0) {
         gpio_.configure(pin.gpio, pin.mode, (output & pin.bit) != 0);
         selected = static_cast<uint8_t>(selected | pin.bit);
      }
   }
   uint8_t value = pinsIn();
   if(wait != 0) {
      // Wait until the selected pins have the output levels.
      const uint32_t start = platform_.nowUs();
      while(((value ^ output) & selected) != 0 && platform_.nowUs() - start < wait) {
         value = pinsIn();
      }
   }
   out.u8(command::swjPins);
   out.u8(value);
   return true;
}

bool Processor::writeAbort(PacketReader &in, PacketWriter &out) {
   in.u8();
   uint32_t data = in.u32();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::writeAbort);
   if(!connected_) {
      out.u8(statusError);
      return true;
   }
   // The address of ABORT is 0.
   static_cast<void>(swd_.transfer(0, data));
   out.u8(statusOk);
   return true;
}

bool Processor::transfer(PacketReader &in, PacketWriter &out) {
   using namespace dap;
   in.u8();
   uint8_t todo = in.u8();
   if(!in.ok()) {
      return false;
   }
   // Check the whole request and the room for the data before the first
   // transfer: a bad request must not reach the target.
   {
      PacketReader check = in;
      std::size_t words = 0;
      for(uint8_t i = 0; i < todo; ++i) {
         const uint8_t request = check.u8();
         // A match read returns no data.
         if((request & requestRead) != 0 && (request & requestMatchValue) == 0) {
            ++words;
         }
         if((request & requestRead) == 0 || (request & requestMatchValue) != 0) {
            check.skip(4);
         }
      }
      if(!check.ok() || words * 4 + 3 > out.room()) {
         return false;
      }
   }
   out.u8(command::transfer);
   const std::size_t head = out.reserve(2);
   uint8_t done = 0;
   uint8_t value = 0;
   bool postedRead = false;
   bool checkWrite = false;
   uint32_t data = 0;
   uint32_t unused = 0;

   while(todo != 0 && connected_ && !aborted()) {
      --todo;
      const uint8_t request = in.u8();
      if((request & requestRead) != 0) {
         // The data words come before any transfer, so that an error
         // skips the right bytes.
         const uint32_t match = (request & requestMatchValue) != 0 ? in.u32() : 0;
         if(postedRead) {
            // Another AP read continues the posted one. Else read the data
            // of the posted read from RDBUFF.
            const bool continued = (request & (requestApNotDp | requestMatchValue)) == requestApNotDp;
            value = swd_.transfer(continued ? request : readRdbuff, data);
            postedRead = continued;
            if(value != ackOk) {
               break;
            }
            out.u32(data);
         }
         if((request & requestMatchValue) != 0) {
            if((request & requestApNotDp) != 0) {
               value = swd_.transfer(request, unused);
               if(value != ackOk) {
                  break;
               }
            }
            uint16_t retries = matchRetries_;
            while(true) {
               value = swd_.transfer(request, data);
               if(value != ackOk) {
                  break;
               }
               if((data & matchMask_) == match) {
                  break;
               }
               if(retries-- == 0 || aborted()) {
                  value |= valueMismatch;
                  break;
               }
            }
            if(value != ackOk) {
               break;
            }
         } else if((request & requestApNotDp) != 0) {
            if(!postedRead) {
               value = swd_.transfer(request, unused);
               if(value != ackOk) {
                  break;
               }
               postedRead = true;
            }
         } else {
            value = swd_.transfer(request, data);
            if(value != ackOk) {
               break;
            }
            out.u32(data);
         }
         checkWrite = false;
      } else {
         uint32_t word = in.u32();
         if(postedRead) {
            value = swd_.transfer(readRdbuff, data);
            if(value != ackOk) {
               break;
            }
            out.u32(data);
            postedRead = false;
         }
         if((request & requestMatchMask) != 0) {
            matchMask_ = word;
            value = ackOk;
         } else {
            value = swd_.transfer(request, word);
            if(value != ackOk) {
               break;
            }
            checkWrite = true;
         }
      }
      if(!in.ok() || !out.ok()) {
         return false;
      }
      ++done;
   }

   // Skip the requests after an error.
   while(todo != 0) {
      --todo;
      const uint8_t request = in.u8();
      if((request & requestRead) == 0 || (request & requestMatchValue) != 0) {
         in.skip(4);
      }
   }
   if(!in.ok()) {
      return false;
   }

   if(value == ackOk) {
      if(postedRead) {
         value = swd_.transfer(readRdbuff, data);
         if(value == ackOk) {
            out.u32(data);
         }
      } else if(checkWrite) {
         value = swd_.transfer(readRdbuff, unused);
      }
   }
   out.set(head, done);
   out.set(head + 1, value);
   return out.ok();
}

bool Processor::transferBlock(PacketReader &in, PacketWriter &out) {
   using namespace dap;
   in.u8();
   uint16_t todo = in.u16();
   uint8_t request = in.u8();
   if(!in.ok()) {
      return false;
   }
   const bool read = (request & requestRead) != 0;
   // Check the data and the room before the first transfer: a bad request
   // must not reach the target.
   const std::size_t dataBytes = static_cast<std::size_t>(todo) * 4;
   if(4 + (read ? dataBytes : 0) > out.room() || (!read && dataBytes > in.remaining())) {
      return false;
   }
   out.u8(command::transferBlock);
   const std::size_t head = out.reserve(3);
   uint16_t done = 0;
   uint8_t value = 0;
   uint32_t data = 0;
   uint32_t unused = 0;

   if(connected_ && todo != 0 && !aborted()) {
      if(read) {
         if((request & requestApNotDp) != 0) {
            // Post the first AP read.
            value = swd_.transfer(request, unused);
         } else {
            value = ackOk;
         }
         while(todo != 0 && value == ackOk && !aborted()) {
            --todo;
            if(todo == 0 && (request & requestApNotDp) != 0) {
               // The last AP read gives the data from RDBUFF.
               request = readRdbuff;
            }
            value = swd_.transfer(request, data);
            if(value != ackOk) {
               break;
            }
            out.u32(data);
            ++done;
         }
         if(todo != 0 && value == ackOk && (request & requestApNotDp) != 0) {
            // An abort stopped the block with an AP read posted. Read its
            // data from RDBUFF, as at the end of the block.
            value = swd_.transfer(readRdbuff, data);
            if(value == ackOk) {
               out.u32(data);
               ++done;
            }
         }
      } else {
         while(todo != 0 && !aborted()) {
            --todo;
            data = in.u32();
            value = swd_.transfer(request, data);
            if(value != ackOk || !in.ok()) {
               break;
            }
            ++done;
         }
         // Check the last write.
         if(value == ackOk) {
            value = swd_.transfer(readRdbuff, unused);
         }
      }
   }
   if(!read) {
      // Skip the data of the writes after an error.
      in.skip(static_cast<std::size_t>(todo) * 4);
   }
   if(!in.ok()) {
      return false;
   }
   out.set(head, static_cast<uint8_t>(done));
   out.set(head + 1, static_cast<uint8_t>(done >> 8));
   out.set(head + 2, value);
   return out.ok();
}

} // namespace cmsisdap
