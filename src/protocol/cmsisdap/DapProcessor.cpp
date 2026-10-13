#include "protocol/cmsisdap/DapProcessor.h"

#include <algorithm>
#include <cstring>

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
                     DapUart *uart)
   : swd_(swd)
   , jtag_(jtag)
   , gpio_(gpio)
   , platform_(platform)
   , info_(info)
   , personalitySwitch_(personalitySwitch)
   , uart_(uart) {
   if(jtag_.available()) {
      info_.capabilities = static_cast<uint16_t>(info_.capabilities | capability::jtag);
   }
   applySettings();
   swd_.setClockHz(clockHz_);
   swd_.setAbort(*this);
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
   if(uart_ != nullptr) {
      uart_->hostGone();
   }
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
   case command::vendor0:
      if(personalitySwitch_ != nullptr) {
         return switchPersonality(in, out);
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

bool Processor::switchPersonality(PacketReader &in, PacketWriter &out) {
   const uint8_t code = in.u8();
   if(!in.ok()) {
      return false;
   }
   out.u8(command::vendor0);
   const auto target = personality::fromCode(code);
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
   std::array<uint8_t, packetSize> rxData{};
   const std::size_t received = uart_->read({ rxData.data(), rxRoom });
   out.u8(command::uartTransfer);
   out.u8(uart_->status());
   out.u16(static_cast<uint16_t>(written));
   out.u16(static_cast<uint16_t>(received));
   out.bytes({ rxData.data(), received });
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
   case infoId::capabilities: {
      // The DapUart is asked here, not in the constructor (see there).
      uint16_t capabilities = info_.capabilities;
      if(uart_ != nullptr) {
         capabilities = static_cast<uint16_t>(capabilities | capability::uartPort);
         if(uart_->hasComPort()) {
            capabilities = static_cast<uint16_t>(capabilities | capability::usbComPort);
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
      // and the SWO buffer
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
   if(port != portDefault && port != portSwd && !jtag) {
      out.u8(0);
      return true;
   }
   if(port_ == Port::Swd) {
      swd_.fini();
   } else if(port_ == Port::Jtag) {
      jtag_.port().fini();
   }
   // Another user of the engine may have changed it.
   applySettings();
   if(jtag) {
      setJtagClock();
      jtag_.port().init();
      // The TAP state and the IR are not known.
      jtag_.invalidate();
      port_ = Port::Jtag;
      out.u8(portJtag);
      return true;
   }
   swd_.setClockHz(clockHz_);
   if((gpio_.available() & protocol::gpio::tdo) != 0) {
      gpio_.configure(protocol::gpio::tdo, protocol::PinMode::Input, false);
   }
   swd_.init();
   port_ = Port::Swd;
   out.u8(portSwd);
   return true;
}

void Processor::disconnect() {
   if(port_ == Port::Swd) {
      swd_.fini();
      port_ = Port::None;
   } else if(port_ == Port::Jtag) {
      jtag_.port().fini();
      port_ = Port::None;
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
   applySettings();
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
   applySettings();
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
   if(port_ == Port::Jtag) {
      setJtagClock();
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
   // TCK and TMS pulses can change the TAP state and the IR.
   jtag_.invalidate();
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

bool Processor::selectDevice(uint8_t index) {
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
