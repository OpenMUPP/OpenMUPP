// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/cmsisdap/DapProcessor.h"

#include <algorithm>
#include <cstring>
#include <optional>

#include "boot/BootloaderEntry.h"
#include "personality/PersonalitySwitch.h"

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

   // The free space, for a writer outside; advance() then takes the bytes.
   [[nodiscard]] art::span<uint8_t> free() const {
      return data_.subspan(size_);
   }

   void advance(std::size_t count) {
      size_ = std::min(size_ + count, data_.size());
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
constexpr uint8_t statusError = 0xFF;

// The ports of DAP_Connect
constexpr uint8_t portDefault = 0;
constexpr uint8_t portSwd = 1;
constexpr uint8_t portJtag = 2;

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
// The SWD pins of DAP_SWJ_Pins: SWCLK/TCK and SWDIO/TMS
constexpr uint8_t swjSwdPins = 0x03;

// The bits of DAP_SWJ_Pins that drive a pin of the board
constexpr uint8_t selectedOutputs(uint16_t available) {
   uint8_t bits = 0;
   for(const SwjPin &pin : swjPinTable) {
      if(pin.output && (available & pin.gpio) != 0) {
         bits = static_cast<uint8_t>(bits | pin.bit);
      }
   }
   return bits;
}

// DAP_HostStatus
constexpr uint8_t hostStatusConnected = 0;
constexpr uint8_t hostStatusRunning = 1;

constexpr char protocolVersion[] = "2.1.0";

} // namespace

Processor::Processor(DapSwd &swd,
                     protocol::GpioPins &gpio,
                     Platform &platform,
                     const DeviceInfo &info,
                     protocol::JtagPort *jtag,
                     personality::PersonalitySwitch *personalitySwitch,
                     DapUart *uart,
                     boot::BootloaderEntry *bootloader,
                     Bridge *bridge,
                     protocol::TargetPower *power)
   : swd_(swd)
   , jtag_(jtag)
   , gpio_(gpio)
   , platform_(platform)
   , info_(info)
   , personalitySwitch_(personalitySwitch)
   , uart_(uart)
   , bootloader_(bootloader)
   , bridge_(bridge)
   , power_(power) {
   if(jtag_.available()) {
      info_.capabilities = static_cast<uint16_t>(info_.capabilities | capability::jtag);
   }
   applySettings();
   swd_.setClockHz(clockHz_);
   swd_.setAbort(*this);
}

bool Processor::claimPins() {
   return pinOwner_ == nullptr || pinOwner_->acquire(pinClient_);
}

bool Processor::ownsPins() const {
   return pinOwner_ == nullptr || pinOwner_->owner() == pinClient_;
}

bool Processor::otherOwnsPins() const {
   return !ownsPins() && pinOwner_->owner() != protocol::PinClient::None;
}

void Processor::applySettings() {
   swd_.apply(settings_);
   jtag_.apply(settings_);
}

// TCK in kHz, rounded down: the JTAG engines take kHz, and 0 is adaptive
// clocking.
void Processor::setJtagClock() {
   static_cast<void>(jtag_.port().setSpeed(std::max<uint32_t>(clockHz_ / 1000u, 1)));
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
   if(vendor_ != nullptr) {
      vendor_->hostGone();
   }
   // The bridge is the JTAG port of the other processor too: not while it
   // owns the pins.
   if(bridge_ != nullptr && !otherOwnsPins()) {
      bridge_->setMode(bridge_->storedMode());
   }
   if(uart_ != nullptr) {
      uart_->hostGone();
   }
   swoReset();
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
   // One byte more than packetSize_ is kept, so execute() sees a request
   // that is too long.
   const std::size_t length = std::min(request.size(), packetSize_ + 1);
   std::copy_n(request.begin(), length, requestBuffer_[index].begin());
   requestLength_[index] = static_cast<uint16_t>(length);
   ++requests_;
}

void Processor::setPacketSize(std::size_t size) {
   packetSize_ = std::clamp(size, fullSpeedPacketSize, maxPacketSize);
}

bool Processor::runNext() {
   if(running_ || requests_ == 0 || mustWait()) {
      return false;
   }
   running_ = true;
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
   running_ = false;
   return true;
}

art::span<uint8_t> Processor::request(std::size_t index) {
   const std::size_t slot = (requestFirst_ + index) % packetCount;
   return { requestBuffer_[slot].data(), requestLength_[slot] };
}

bool Processor::isQueued(std::size_t index) {
   const auto packet = request(index);
   return packet[0] == command::queueCommands && packet.size() <= packetSize_;
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
   PacketWriter out({ responseBuffer_[slot].data(), packetSize_ });
   if(request.size() > packetSize_) {
      out.u8(command::invalid);
   } else {
      PacketReader in(request);
      if(!processCommand(in, out, false) || !out.ok()) {
         out.truncate(0);
         out.u8(command::invalid);
      }
   }
   responseLength_[slot] = static_cast<uint16_t>(out.size());
   ++responses_;
}

bool Processor::processCommand(PacketReader &in, PacketWriter &out, bool nested) {
   const uint8_t id = in.u8();
   if(!in.ok()) {
      return false;
   }
   if(vendor_ != nullptr && id >= command::vendor0 && id <= command::vendor2) {
      return vendorCommand(id, in, out, nested);
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
   case command::jtagSequence:
      return jtagSequence(in, out);
   case command::jtagConfigure:
      return jtagConfigure(in, out);
   case command::jtagIdcode:
      return jtagIdcode(in, out);
   case command::uartTransport:
   case command::uartConfigure:
   case command::uartTransfer:
   case command::uartControl:
   case command::uartStatus:
      if(uart_ != nullptr) {
         return uartCommand(id, in, out);
      }
      out.u8(command::invalid);
      return true;
   case command::swoTransport:
   case command::swoMode:
   case command::swoBaudrate:
   case command::swoControl:
   case command::swoStatus:
   case command::swoData:
   case command::swoExtendedStatus:
      if(swo_ != nullptr) {
         return swoCommand(id, in, out);
      }
      out.u8(command::invalid);
      return true;
   case command::vendor0:
      if(personalitySwitch_ != nullptr || bootloader_ != nullptr) {
         return switchPersonality(in, out);
      }
      out.u8(command::invalid);
      return true;
   case command::vendor1:
      if(bridge_ != nullptr) {
         return switchBridge(in, out);
      }
      out.u8(command::invalid);
      return true;
   case command::vendor2:
      if(power_ != nullptr) {
         return targetPower(in, out);
      }
      out.u8(command::invalid);
      return true;
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

bool Processor::vendorCommand(uint8_t id, PacketReader &in, PacketWriter &out, bool nested) {
   const auto fields = in.bytes(in.remaining());
   const std::size_t length = nested ? 0 : vendor_->command(id, fields, out.free());
   if(length == 0) {
      out.u8(command::invalid);
   } else {
      out.advance(length);
   }
   return true;
}

bool Processor::switchPersonality(PacketReader &in, PacketWriter &out) {
   const uint8_t code = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::vendor0);
   if(code == boot::bootloaderCode && bootloader_ != nullptr) {
      // The main loop enters the bootloader after the reply delay
      bootloader_->request();
      out.u8(statusOk);
      return true;
   }
   const auto target = personalitySwitch_ != nullptr ? personality::fromCode(code) : std::nullopt;
   if(!target) {
      out.u8(statusError);
      return true;
   }
   // The main loop stores the personality and resets after the reply
   // delay: the transport sends this response first.
   personalitySwitch_->request(*target);
   out.u8(statusOk);
   return true;
}

bool Processor::switchBridge(PacketReader &in, PacketWriter &out) {
   const uint8_t value = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::vendor1);
   const auto code = static_cast<uint8_t>(value & ~bridgeStoreFlag);
   // The stored mode is stored already: no store flag with it
   // The bridge is the JTAG port of the other processor too: not while it
   // owns the pins.
   if(code > bridgeStoredMode || value == (bridgeStoredMode | bridgeStoreFlag) || otherOwnsPins()) {
      out.u8(statusError);
      return true;
   }
   const auto mode = code == bridgeStoredMode ? bridge_->storedMode() : static_cast<BridgeMode>(code);
   bridge_->setMode(mode);
   // The other port has another TAP state and IR.
   jtag_.invalidate();
   const bool stored = (value & bridgeStoreFlag) == 0 || bridge_->storeMode(mode);
   out.u8(stored ? statusOk : statusError);
   return true;
}

// [0x82][op u8] or [0x82][3][mV u16] -> [0x82][status u8][mV u16][state u8]
bool Processor::targetPower(PacketReader &in, PacketWriter &out) {
   constexpr uint8_t opOff = 0;
   constexpr uint8_t opOn = 1;
   constexpr uint8_t opRead = 2;
   constexpr uint8_t opSelect = 3;
   constexpr uint8_t statusRefused = 1;
   constexpr uint8_t stateFault = 0x80;
   const uint8_t op = in.u8();
   // Only op 3 has the voltage: the old requests keep their length.
   const uint16_t selectMv = op == opSelect ? in.u16() : 0;
   // The room before the switch: a response that does not fit changes
   // nothing.
   if(!in.ok() || out.room() < 5) {
      return false;
   }
   std::optional<protocol::Supply> supply;
   if(op == opOn || (op == opSelect && selectMv == 3300)) {
      supply = protocol::Supply::V3v3;
   } else if(op == opSelect && selectMv == 5000) {
      supply = protocol::Supply::V5;
   }
   uint8_t status = statusOk;
   if(supply && otherOwnsPins()) {
      // Only the owner of the pins, or anyone while nobody owns them
      status = statusRefused;
   } else if(supply) {
      switch(power_->switchOn(*supply)) {
      case protocol::TargetPower::Result::Ok:
         break;
      case protocol::TargetPower::Result::Refused:
         status = statusRefused;
         break;
      case protocol::TargetPower::Result::NotSupported:
         status = statusError;
         break;
      }
   } else if(op == opOff) {
      if(power_->hardware().hasSwitch()) {
         power_->switchOff();
      } else {
         status = statusError;
      }
   } else if(op != opRead) {
      status = statusError;
   }
   // First: it also takes a fault that the main loop did not take yet.
   const bool fault = power_->takeFaultReport();
   const auto state = static_cast<uint8_t>(static_cast<uint8_t>(power_->supply()) | (fault ? stateFault : 0));
   out.u8(command::vendor2);
   out.u8(status);
   out.u16(power_->millivolts());
   out.u8(state);
   return true;
}

bool Processor::uartCommand(uint8_t id, PacketReader &in, PacketWriter &out) {
   if(id == command::uartTransfer) {
      return uartTransfer(in, out);
   }
   if(id == command::uartStatus) {
      out.u8(id);
      out.u8(uart_->status());
      out.u32(uart_->rxCount());
      out.u32(uart_->txCount());
      return true;
   }
   const uint8_t value = in.u8();
   if(id == command::uartConfigure) {
      uint32_t baudRate = in.u32();
      if(!in.ok()) {
         return false;
      }
      out.u8(id);
      out.u8(uart_->configure(value, baudRate));
      out.u32(baudRate);
      return true;
   }
   if(!in.ok()) {
      return false;
   }
   out.u8(id);
   const bool ok = id == command::uartTransport ? uart_->setTransport(value) : uart_->control(value);
   out.u8(ok ? statusOk : statusError);
   return true;
}

// [0x21][Rx Count u16][Tx Count u16][data] ->
// [0x21][status][Tx Count u16][Rx Count u16][data]
bool Processor::uartTransfer(PacketReader &in, PacketWriter &out) {
   const uint16_t rxWanted = in.u16();
   const uint16_t txCount = in.u16();
   const auto txData = in.bytes(txCount);
   if(!in.ok()) {
      return false;
   }
   constexpr std::size_t header = 6;
   if(out.room() < header) {
      return false;
   }
   const std::size_t written = uart_->write(txData);
   const std::size_t rxRoom = std::min<std::size_t>(rxWanted, out.room() - header);
   std::array<uint8_t, maxPacketSize> rxData{};
   const std::size_t received = uart_->read({ rxData.data(), rxRoom });
   out.u8(command::uartTransfer);
   out.u8(uart_->status());
   out.u16(static_cast<uint16_t>(written));
   out.u16(static_cast<uint16_t>(received));
   out.bytes({ rxData.data(), received });
   return true;
}

void Processor::swoReset() {
   if(swo_ != nullptr) {
      swo_->stop();
   }
   swoTransport_ = swoTransport::none;
   swoMode_ = swoMode::off;
   swoBaud_ = 0;
}

uint8_t Processor::swoTraceStatus() {
   const DapSwo::Status status = swo_->status();
   swo_->clearErrors();
   uint8_t bits = status.active ? swoStatus::active : 0;
   if(status.framingError) {
      bits = static_cast<uint8_t>(bits | swoStatus::error);
   }
   if(status.overrun) {
      bits = static_cast<uint8_t>(bits | swoStatus::overrun);
   }
   return bits;
}

bool Processor::swoCommand(uint8_t id, PacketReader &in, PacketWriter &out) {
   if(id == command::swoData) {
      return swoData(in, out);
   }
   if(id == command::swoStatus) {
      out.u8(id);
      out.u8(swoTraceStatus());
      out.u32(swo_->status().count);
      return true;
   }
   if(id == command::swoBaudrate) {
      const uint32_t baud = in.u32();
      if(!in.ok()) {
         return false;
      }
      out.u8(id);
      swoBaud_ = swoMode_ != swoMode::off ? swo_->realBaud(swoMode_, baud) : 0;
      if(swoBaud_ == 0) {
         swo_->stop();
      } else if(swo_->active()) {
         static_cast<void>(swo_->setBaud(baud));
      }
      out.u32(swoBaud_);
      return true;
   }
   const uint8_t value = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(id);
   if(id == command::swoExtendedStatus) {
      if((value & swoExtended::status) != 0) {
         out.u8(swoTraceStatus());
      }
      if((value & swoExtended::count) != 0) {
         out.u32(swo_->status().count);
      }
      return true;
   }
   bool ok = false;
   if(id == command::swoControl) {
      const bool start = (value & swoControl::start) != 0;
      if(!start) {
         swo_->stop();
         ok = true;
      } else {
         ok = swo_->active() ||
              (swoMode_ != swoMode::off && swoBaud_ != 0 && port_ != Port::Jtag && swo_->start(swoMode_, swoBaud_) != 0);
      }
   } else if(id == command::swoTransport) {
      ok = !swo_->active() && (value == swoTransport::none || value == swoTransport::dataCommand ||
                               (value == swoTransport::streaming && swoStreaming_));
      if(ok) {
         swoTransport_ = value;
      }
   } else {
      // DAP_SWO_Mode stops the capture, and the baud rate must be set
      // again. A mode that is not available sets mode off.
      swo_->stop();
      swoBaud_ = 0;
      ok = value == swoMode::off || swo_->hasMode(value);
      swoMode_ = ok ? value : swoMode::off;
   }
   out.u8(ok ? statusOk : statusError);
   return true;
}

// [0x1C][count u16] -> [0x1C][status][count u16][data]
bool Processor::swoData(PacketReader &in, PacketWriter &out) {
   const uint16_t wanted = in.u16();
   if(!in.ok()) {
      return false;
   }
   constexpr std::size_t header = 4;
   if(out.room() < header) {
      return false;
   }
   out.u8(command::swoData);
   // The status before the data, as in the reference implementation
   out.u8(swoTraceStatus());
   const std::size_t countAt = out.reserve(2);
   std::size_t received = 0;
   if(swoTransport_ == swoTransport::dataCommand) {
      received = swo_->read(out.free().first(std::min<std::size_t>(wanted, out.room())));
      out.advance(received);
   }
   out.set(countAt, static_cast<uint8_t>(received));
   out.set(countAt + 1, static_cast<uint8_t>(received >> 8));
   return true;
}

bool Processor::infoCommand(PacketReader &in, PacketWriter &out) {
   const uint8_t id = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::info);
   const auto text = [&out](const char *value) {
      const std::size_t length = value == nullptr ? 0 : std::min(std::strlen(value), fullSpeedPacketSize - 3);
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
   case infoId::capabilities: {
      // The DapUart is asked here, not in the constructor (see there).
      uint16_t capabilities = info_.capabilities;
      if(uart_ != nullptr) {
         capabilities = static_cast<uint16_t>(capabilities | capability::uartPort);
         if(uart_->hasComPort()) {
            capabilities = static_cast<uint16_t>(capabilities | capability::usbComPort);
         }
      }
      if(swo_ != nullptr) {
         if(swo_->hasMode(swoMode::uart)) {
            capabilities = static_cast<uint16_t>(capabilities | capability::swoUart);
         }
         if(swo_->hasMode(swoMode::manchester)) {
            capabilities = static_cast<uint16_t>(capabilities | capability::swoManchester);
         }
         if(swoStreaming_) {
            capabilities = static_cast<uint16_t>(capabilities | capability::swoStreaming);
         }
      }
      out.u8(capabilities > 0xFF ? 2 : 1);
      out.u8(static_cast<uint8_t>(capabilities));
      if(capabilities > 0xFF) {
         out.u8(static_cast<uint8_t>(capabilities >> 8));
      }
      break;
   }
   case infoId::uartRxBufferSize:
   case infoId::uartTxBufferSize:
      if(uart_ == nullptr) {
         out.u8(0);
         break;
      }
      out.u8(4);
      out.u32(id == infoId::uartRxBufferSize ? uart_->rxBufferSize() : uart_->txBufferSize());
      break;
   case infoId::swoBufferSize:
      if(swo_ == nullptr) {
         out.u8(0);
         break;
      }
      out.u8(4);
      out.u32(swo_->capacity());
      break;
   case infoId::packetCount:
      out.u8(1);
      out.u8(static_cast<uint8_t>(packetCount));
      break;
   case infoId::packetSize:
      out.u8(2);
      out.u16(static_cast<uint16_t>(packetSize_));
      break;
   default:
      // Not available: the target device and board, and the test domain
      // timer
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
   const bool jtag = port == portJtag && jtag_.available();
   if((port != portDefault && port != portSwd && !jtag) || !claimPins()) {
      out.u8(0);
      return true;
   }
   if(portActive_ && port_ == Port::Swd) {
      swd_.fini();
   } else if(portActive_ && port_ == Port::Jtag) {
      jtag_.port().fini();
   }
   portActive_ = false;
   // Another user of the engine may have changed it.
   applySettings();
   swd_.setAbort(*this);
   if(jtag) {
      // SWO is for SWD. The Manchester capture of the CH32V307 also uses
      // the DMA channel of the JTAG fast path (swo/SwoReceiver.h).
      if(swo_ != nullptr) {
         swo_->stop();
      }
      setJtagClock();
      // The TAP state and the IR are not known.
      jtag_.invalidate();
      port_ = Port::Jtag;
      out.u8(portJtag);
      return true;
   }
   swd_.setClockHz(clockHz_);
   port_ = Port::Swd;
   out.u8(portSwd);
   return true;
}

// DAP_Connect only sets the owner and the mode. The pins change here, at the
// first command that uses the port: on some boards, driving them also turns
// on the level shifters.
void Processor::activatePort() {
   if(portActive_ || port_ == Port::None) {
      return;
   }
   portActive_ = true;
   if(port_ == Port::Jtag) {
      jtag_.port().init();
      return;
   }
   if((gpio_.available() & protocol::gpio::tdo) != 0) {
      gpio_.configure(protocol::gpio::tdo, protocol::PinMode::Input, false);
   }
   swd_.init();
}

void Processor::disconnect() {
   if(!ownsPins()) {
      // The other processor owns the pins. This one has no port: DAP_Connect
      // needs the pins.
      return;
   }
   if(vendor_ != nullptr) {
      vendor_->releasePins();
   }
   if(portActive_ && port_ == Port::Swd) {
      swd_.fini();
   } else if(portActive_ && port_ == Port::Jtag) {
      jtag_.port().fini();
   } else {
      // DAP_SWJ_Pins can drive SWCLK and SWDIO without a connection.
      for(const uint16_t bit : { protocol::gpio::tck, protocol::gpio::tms }) {
         if((gpio_.available() & bit) != 0) {
            gpio_.configure(bit, protocol::PinMode::Input, false);
         }
      }
   }
   portActive_ = false;
   port_ = Port::None;
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
   if(pinOwner_ != nullptr) {
      pinOwner_->release(pinClient_);
   }
}

bool Processor::transferConfigure(PacketReader &in, PacketWriter &out) {
   settings_.idleCycles = in.u8();
   settings_.waitRetries = in.u16();
   matchRetries_ = in.u16();
   if(!in.ok()) {
      return false;
   }
   if(ownsPins()) {
      applySettings();
   }
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
   if(ownsPins()) {
      applySettings();
   }
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
   if(ownsPins()) {
      swd_.setClockHz(hz);
      if(port_ == Port::Jtag) {
         setJtagClock();
      }
   }
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
   activatePort();
   if(port_ == Port::Jtag) {
      // The sequence is on TMS.
      out.u8(jtag_.tmsSequence(static_cast<uint16_t>(bits), data) ? statusOk : statusError);
      return true;
   }
   if(port_ != Port::Swd) {
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
   const bool swd = port_ == Port::Swd;
   if(!swd) {
      out.set(statusPosition, statusError);
   } else {
      activatePort();
   }
   for(uint8_t i = 0; i < count; ++i) {
      // A cycle count of 0 is 64 cycles. Bit 7: read SWDIO.
      const uint8_t info = in.u8();
      const std::size_t bits = (info & 0x3Fu) == 0 ? 64 : (info & 0x3Fu);
      const std::size_t bytes = (bits + 7) / 8;
      if((info & 0x80u) != 0) {
         if(swd) {
            std::array<uint8_t, 8> data{};
            swd_.sequenceIn(static_cast<uint16_t>(bits), { data.data(), bytes });
            out.bytes({ data.data(), bytes });
         }
      } else {
         const auto data = in.bytes(bytes);
         if(swd && in.ok()) {
            swd_.sequenceOut(static_cast<uint16_t>(bits), data);
         }
      }
      if(!in.ok()) {
         return false;
      }
   }
   return true;
}

bool Processor::jtagSequence(PacketReader &in, PacketWriter &out) {
   const uint8_t count = in.u8();
   if(!in.ok()) {
      return false;
   }
   // Check the whole request and the room for the TDO data before the
   // first sequence.
   {
      PacketReader check = in;
      std::size_t inputBytes = 0;
      for(uint8_t i = 0; i < count; ++i) {
         const uint8_t info = check.u8();
         const std::size_t bytes = ((((info & 0x3Fu) == 0) ? 64 : (info & 0x3Fu)) + 7) / 8;
         check.skip(bytes);
         if((info & 0x80u) != 0) {
            inputBytes += bytes;
         }
      }
      if(!check.ok() || inputBytes + 2 > out.room()) {
         return false;
      }
   }
   out.u8(command::jtagSequence);
   const std::size_t statusPosition = out.reserve(1);
   const bool jtag = port_ == Port::Jtag;
   if(!jtag) {
      out.set(statusPosition, statusError);
   } else {
      activatePort();
   }
   for(uint8_t i = 0; i < count; ++i) {
      // A cycle count of 0 is 64 cycles. Bit 6: TMS. Bit 7: read TDO.
      const uint8_t info = in.u8();
      const std::size_t bits = (info & 0x3Fu) == 0 ? 64 : (info & 0x3Fu);
      const std::size_t bytes = (bits + 7) / 8;
      const auto tdi = in.bytes(bytes);
      if(!in.ok()) {
         return false;
      }
      if(jtag) {
         const bool capture = (info & 0x80u) != 0;
         std::array<uint8_t, 8> tdo{};
         if(!jtag_.sequence(static_cast<uint8_t>(bits), (info & 0x40u) != 0, tdi, { tdo.data(), capture ? bytes : 0 })) {
            out.set(statusPosition, statusError);
         }
         if(capture) {
            out.bytes({ tdo.data(), bytes });
         }
      }
   }
   return true;
}

bool Processor::jtagConfigure(PacketReader &in, PacketWriter &out) {
   const uint8_t count = in.u8();
   const auto irLengths = in.bytes(count);
   if(!in.ok()) {
      return false;
   }
   out.u8(command::jtagConfigure);
   out.u8(jtag_.available() && jtag_.configure(irLengths) ? statusOk : statusError);
   return true;
}

bool Processor::jtagIdcode(PacketReader &in, PacketWriter &out) {
   const uint8_t index = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::jtagIdcode);
   uint32_t value = 0;
   if(port_ == Port::Jtag) {
      activatePort();
   }
   if(port_ != Port::Jtag || !jtag_.select(index) || !jtag_.readIdcode(value)) {
      out.u8(statusError);
      return true;
   }
   out.u8(statusOk);
   out.u32(value);
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
   if(!claimPins()) {
      out.u8(statusError);
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
   // Without the pins, the command only reads them.
   const bool drive = (select & selectedOutputs(gpio_.available())) == 0 || claimPins();
   if(drive) {
      // TCK and TMS pulses can change the TAP state and the IR.
      jtag_.invalidate();
   }
   uint8_t selected = 0;
   for(const SwjPin &pin : swjPinTable) {
      if(drive && pin.output && (select & pin.bit) != 0 && (gpio_.available() & pin.gpio) != 0) {
         gpio_.configure(pin.gpio, pin.mode, (output & pin.bit) != 0);
         selected = static_cast<uint8_t>(selected | pin.bit);
      }
   }
   if(bridge_ != nullptr && (selected & swjSwdPins) != 0) {
      bridge_->pinsChanged();
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

bool Processor::selectDevice(uint8_t index) {
   activatePort();
   if(port_ == Port::Jtag) {
      return jtag_.select(index);
   }
   // SWD ignores the index.
   return port_ == Port::Swd;
}

uint8_t Processor::dpTransfer(uint8_t request, uint32_t &data) {
   const uint8_t value = port_ == Port::Jtag ? jtag_.transfer(request, data) : swd_.transfer(request, data);
   // The host stopped the WAIT retries: not an error of the wire.
   if(value != dap::ackOk && !(value == dap::ackWait && aborted())) {
      ++errorCount_;
   }
   return value;
}

bool Processor::writeAbort(PacketReader &in, PacketWriter &out) {
   const uint8_t index = in.u8();
   uint32_t data = in.u32();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::writeAbort);
   if(!selectDevice(index)) {
      out.u8(statusError);
      return true;
   }
   if(port_ == Port::Jtag) {
      jtag_.writeAbort(data);
   } else {
      // The address of ABORT is 0.
      static_cast<void>(swd_.transfer(0, data));
   }
   out.u8(statusOk);
   return true;
}

bool Processor::transfer(PacketReader &in, PacketWriter &out) {
   using namespace dap;
   const uint8_t index = in.u8();
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
   // A device that is not in the chain gives no transfer and the value 0.
   const bool ready = selectDevice(index);
   uint8_t done = 0;
   uint8_t value = 0;
   bool postedRead = false;
   bool checkWrite = false;
   uint32_t data = 0;
   uint32_t unused = 0;

   while(todo != 0 && ready && !aborted()) {
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
            value = dpTransfer(continued ? request : readRdbuff, data);
            postedRead = continued;
            if(value != ackOk) {
               break;
            }
            out.u32(data);
         }
         if((request & requestMatchValue) != 0) {
            if((request & requestApNotDp) != 0) {
               value = dpTransfer(request, unused);
               if(value != ackOk) {
                  break;
               }
            }
            uint16_t retries = matchRetries_;
            while(true) {
               value = dpTransfer(request, data);
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
               value = dpTransfer(request, unused);
               if(value != ackOk) {
                  break;
               }
               postedRead = true;
            }
         } else {
            value = dpTransfer(request, data);
            if(value != ackOk) {
               break;
            }
            out.u32(data);
         }
         checkWrite = false;
      } else {
         uint32_t word = in.u32();
         if(postedRead) {
            value = dpTransfer(readRdbuff, data);
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
            value = dpTransfer(request, word);
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
         value = dpTransfer(readRdbuff, data);
         if(value == ackOk) {
            out.u32(data);
         }
      } else if(checkWrite) {
         value = dpTransfer(readRdbuff, unused);
      }
   }
   out.set(head, done);
   out.set(head + 1, value);
   return out.ok();
}

bool Processor::transferBlock(PacketReader &in, PacketWriter &out) {
   using namespace dap;
   const uint8_t index = in.u8();
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

   if(todo != 0 && !aborted() && selectDevice(index)) {
      if(read) {
         if((request & requestApNotDp) != 0) {
            // Post the first AP read.
            value = dpTransfer(request, unused);
         } else {
            value = ackOk;
         }
         while(todo != 0 && value == ackOk && !aborted()) {
            --todo;
            if(todo == 0 && (request & requestApNotDp) != 0) {
               // The last AP read gives the data from RDBUFF.
               request = readRdbuff;
            }
            value = dpTransfer(request, data);
            if(value != ackOk) {
               break;
            }
            out.u32(data);
            ++done;
         }
         if(todo != 0 && value == ackOk && (request & requestApNotDp) != 0) {
            // An abort stopped the block with an AP read posted. Read its
            // data from RDBUFF, as at the end of the block.
            value = dpTransfer(readRdbuff, data);
            if(value == ackOk) {
               out.u32(data);
               ++done;
            }
         }
      } else {
         while(todo != 0 && !aborted()) {
            --todo;
            data = in.u32();
            value = dpTransfer(request, data);
            if(value != ackOk || !in.ok()) {
               break;
            }
            ++done;
         }
         // Check the last write.
         if(value == ackOk) {
            value = dpTransfer(readRdbuff, unused);
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
