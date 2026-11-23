// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/avr/EdbgTransport.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace avr {

namespace {

// [0x0E][version][sequence u16][handler]
constexpr std::size_t commandHeaderBytes = 5;
// [0x0E][sequence u16][handler]
constexpr std::size_t responseHeaderBytes = 4;
// [id][fragment][size u16]
constexpr std::size_t fragmentHeaderBytes = 4;
constexpr uint8_t maxFragments = 15;

// The commands of the scopes
constexpr uint8_t cmdQuery = 0x00;
constexpr uint8_t cmdSet = 0x01;
constexpr uint8_t cmdGet = 0x02;
constexpr uint8_t cmdStartSession = 0x10;
constexpr uint8_t cmdEndSession = 0x11;

// Discovery QUERY contexts
constexpr uint8_t queryHandlers = 0x00;
constexpr uint8_t queryToolName = 0x80;
constexpr uint8_t querySerialNumber = 0x81;

// SET/GET contexts and parameters
constexpr uint8_t hkContextConfig = 0x00;
constexpr uint8_t hkContextAnalog = 0x01;
constexpr uint8_t edbgContextControl = 0x00;
constexpr uint8_t edbgTargetPower = 0x10;

// Failure codes
constexpr uint8_t notSupported = 0x10;
constexpr uint8_t edbgNotSupported = 0x01;
constexpr uint8_t edbgIllegalAction = 0x16;

constexpr std::array<uint8_t, 5> handlers{ edbg::handler::discovery, edbg::handler::housekeeping, edbg::handler::avrIsp,
                                           edbg::handler::avr8, edbg::handler::edbgControl };

// [cmd][version][context][address][bytes]
struct ParameterAccess {
   uint8_t context;
   uint8_t address;
   uint8_t bytes;
   art::span<const uint8_t> data;
};

// A SET or GET. No value when the command is too short, or a SET has less
// data than its byte count.
std::optional<ParameterAccess> parameterAccess(art::span<const uint8_t> payload) {
   constexpr std::size_t headerBytes = 5;
   if(payload.size() < headerBytes) {
      return std::nullopt;
   }
   ParameterAccess access{ .context = payload[2], .address = payload[3], .bytes = payload[4], .data = {} };
   if(payload[0] == cmdSet) {
      if(payload.size() - headerBytes < access.bytes) {
         return std::nullopt;
      }
      access.data = payload.subspan(headerBytes, access.bytes);
   }
   return access;
}

// The parameters [address, address + bytes) of a table: empty when the
// range is empty or not in the table.
art::span<uint8_t> parameterRange(art::span<uint8_t> table, const ParameterAccess &access) {
   if(access.bytes == 0 || std::size_t{ access.address } + access.bytes > table.size()) {
      return {};
   }
   return table.subspan(access.address, access.bytes);
}

} // namespace

// Writes a scope response. A write past the end is lost and clears ok().
class EdbgTransport::Writer {
   public:
   explicit Writer(art::span<uint8_t> data)
      : data_(data) {
   }

   void u8(uint8_t value) {
      if(size_ >= data_.size()) {
         ok_ = false;
         return;
      }
      data_[size_++] = value;
   }

   void bytes(art::span<const uint8_t> values) {
      for(const uint8_t value : values) {
         u8(value);
      }
   }

   void text(const char *value) {
      if(value != nullptr) {
         bytes({ reinterpret_cast<const uint8_t *>(value), std::strlen(value) });
      }
   }

   void ok() {
      u8(edbg::response::ok);
      u8(0);
   }

   void list(art::span<const uint8_t> values) {
      u8(edbg::response::list);
      u8(0);
      bytes(values);
   }

   void listText(const char *value) {
      u8(edbg::response::list);
      u8(0);
      text(value);
   }

   // Version 1: with the trailing status
   void data(art::span<const uint8_t> values) {
      u8(edbg::response::data);
      u8(1);
      bytes(values);
      u8(0);
   }

   void failed(uint8_t code) {
      u8(edbg::response::failed);
      u8(0);
      u8(code);
   }

   // The free space, for a writer outside; advance() then takes the bytes.
   [[nodiscard]] art::span<uint8_t> free() const {
      return data_.subspan(size_);
   }

   void advance(std::size_t count) {
      size_ = std::min(size_ + count, data_.size());
   }

   [[nodiscard]] std::size_t size() const {
      return size_;
   }

   [[nodiscard]] bool isOk() const {
      return ok_;
   }

   private:
   art::span<uint8_t> data_;
   std::size_t size_{ 0 };
   bool ok_{ true };
};

EdbgTransport::EdbgTransport(const ToolInfo &info,
                             IspHandler &isp,
                             Avr8Handler &avr8,
                             const cmsisdap::Processor &dap,
                             protocol::TargetPower *power,
                             protocol::TargetPinOwner *owner)
   : info_(info)
   , isp_(isp)
   , avr8_(avr8)
   , dap_(dap)
   , power_(power)
   , owner_(owner) {
}

std::size_t EdbgTransport::command(uint8_t id, art::span<const uint8_t> fields, art::span<uint8_t> response) {
   // The frame of the last AVR_CMD runs now, after its acknowledge went to
   // the host (AVaRICE reads it for 200 ms only, issue #112).
   if(framePending_) {
      runFrame();
      dropFrame();
   }
   switch(id) {
   case edbg::avrCmd:
      return avrCommand(fields, response);
   case edbg::avrRsp:
      return avrResponse(response);
   case edbg::avrEvt:
      return avrEvent(response);
   default:
      return 0;
   }
}

std::size_t EdbgTransport::avrCommand(art::span<const uint8_t> fields, art::span<uint8_t> response) {
   if(response.size() < 2) {
      return 0;
   }
   response[0] = edbg::avrCmd;
   response[1] = edbg::fragmentRejected;
   if(fields.size() < fragmentHeaderBytes - 1) {
      dropFrame();
      return 2;
   }
   const uint8_t number = fields[0] >> 4;
   const uint8_t count = fields[0] & 0x0F;
   const std::size_t size = (std::size_t{ fields[1] } << 8) | fields[2];
   if(number == 1) {
      // A new frame: the host gave up the last one.
      dropFrame();
      responseLength_ = 0;
      fragments_ = count;
      nextFragment_ = 1;
   }
   if(number == 0 || count == 0 || number > count || number != nextFragment_ || count != fragments_ ||
      size > fields.size() - (fragmentHeaderBytes - 1) || size > frame_.size() - frameLength_) {
      dropFrame();
      return 2;
   }
   std::copy_n(fields.begin() + (fragmentHeaderBytes - 1), size, frame_.begin() + static_cast<std::ptrdiff_t>(frameLength_));
   frameLength_ += size;
   if(number < count) {
      ++nextFragment_;
      response[1] = edbg::fragmentMore;
      return 2;
   }
   // The next vendor command runs the frame.
   framePending_ = true;
   response[1] = edbg::fragmentDone;
   return 2;
}

std::size_t EdbgTransport::avrResponse(art::span<uint8_t> response) {
   if(response.size() <= fragmentHeaderBytes) {
      return 0;
   }
   const std::size_t chunk = response.size() - fragmentHeaderBytes;
   const std::size_t count = (responseLength_ + chunk - 1) / chunk;
   response[0] = edbg::avrRsp;
   if(responseLength_ == 0 || count > maxFragments) {
      responseLength_ = 0;
      response[1] = 0;
      response[2] = 0;
      response[3] = 0;
      return fragmentHeaderBytes;
   }
   const std::size_t offset = responseSent_ * chunk;
   const std::size_t size = std::min(chunk, responseLength_ - offset);
   ++responseSent_;
   response[1] = static_cast<uint8_t>(responseSent_ << 4 | count);
   response[2] = static_cast<uint8_t>(size >> 8);
   response[3] = static_cast<uint8_t>(size);
   std::copy_n(response_.begin() + static_cast<std::ptrdiff_t>(offset), size, response.begin() + fragmentHeaderBytes);
   if(responseSent_ == count) {
      responseLength_ = 0;
   }
   return fragmentHeaderBytes + size;
}

std::size_t EdbgTransport::avrEvent(art::span<uint8_t> response) {
   constexpr std::size_t eventHeaderBytes = 3;
   if(response.size() < eventHeaderBytes) {
      return 0;
   }
   const std::size_t size = avr8_.event(response.subspan(eventHeaderBytes));
   response[0] = edbg::avrEvt;
   response[1] = static_cast<uint8_t>(size >> 8);
   response[2] = static_cast<uint8_t>(size);
   return eventHeaderBytes + size;
}

void EdbgTransport::releaseIdlePins() {
   if(owner_ != nullptr && !isp_.programming() && !avr8_.active() && !dap_.connected()) {
      owner_->release(protocol::PinClient::CmsisDapHid);
   }
}

void EdbgTransport::dropFrame() {
   framePending_ = false;
   frameLength_ = 0;
   fragments_ = 0;
   nextFragment_ = 0;
}

void EdbgTransport::runFrame() {
   responseLength_ = 0;
   responseSent_ = 0;
   if(frameLength_ < commandHeaderBytes || frame_[0] != edbg::sof || frame_[1] != 0) {
      return;
   }
   const uint8_t handler = frame_[4];
   response_[0] = edbg::sof;
   response_[1] = frame_[2];
   response_[2] = frame_[3];
   response_[3] = handler;
   const art::span<const uint8_t> payload(frame_.data() + commandHeaderBytes, frameLength_ - commandHeaderBytes);
   Writer out({ response_.data() + responseHeaderBytes, response_.size() - responseHeaderBytes });
   switch(handler) {
   case edbg::handler::discovery:
      discovery(payload, out);
      break;
   case edbg::handler::housekeeping:
      housekeeping(payload, out);
      break;
   case edbg::handler::avrIsp:
      avrIsp(payload, out);
      break;
   case edbg::handler::avr8:
      avr8(payload, out);
      break;
   case edbg::handler::edbgControl:
      edbgControl(payload, out);
      break;
   default:
      out.failed(notSupported);
      break;
   }
   // Each response is a few bytes, or an ISP read that fits.
   responseLength_ = out.isOk() ? responseHeaderBytes + out.size() : 0;
}

void EdbgTransport::discovery(art::span<const uint8_t> payload, Writer &out) {
   if(payload.size() < 3 || payload[0] != cmdQuery) {
      out.failed(notSupported);
      return;
   }
   switch(payload[2]) {
   case queryHandlers:
      out.list(handlers);
      break;
   case queryToolName:
      out.listText(info_.name);
      break;
   case querySerialNumber:
      out.listText(info_.serialNumber);
      break;
   default:
      out.failed(notSupported);
      break;
   }
}

void EdbgTransport::housekeeping(art::span<const uint8_t> payload, Writer &out) {
   if(payload.empty()) {
      out.failed(notSupported);
      return;
   }
   switch(payload[0]) {
   case cmdStartSession:
   case cmdEndSession:
      out.ok();
      return;
   case cmdGet:
      break;
   default:
      // SET too: the parameters are read-only.
      out.failed(notSupported);
      return;
   }
   const auto access = parameterAccess(payload);
   std::array<uint8_t, 5> config{ info_.hardwareVersion, info_.firmwareMajor, info_.firmwareMinor,
                                  static_cast<uint8_t>(info_.firmwareBuild), static_cast<uint8_t>(info_.firmwareBuild >> 8) };
   const uint16_t millivolts = power_ != nullptr ? power_->millivolts() : 0;
   std::array<uint8_t, 2> analog{ static_cast<uint8_t>(millivolts), static_cast<uint8_t>(millivolts >> 8) };
   art::span<uint8_t> values;
   if(access && access->context == hkContextConfig) {
      values = parameterRange(config, *access);
   } else if(access && access->context == hkContextAnalog) {
      values = parameterRange(analog, *access);
   }
   if(values.empty()) {
      out.failed(notSupported);
   } else {
      out.data(values);
   }
}

namespace {

// A new reading of the adapter at each start of an AVR interface
bool gateAllows(protocol::TargetGate *gate) {
   return gate == nullptr || gate->check(protocol::TargetProtocol::Avr);
}

} // namespace

void EdbgTransport::avrIsp(art::span<const uint8_t> payload, Writer &out) {
   constexpr protocol::PinClient client = protocol::PinClient::CmsisDapHid;
   const uint8_t id = payload.empty() ? 0 : payload[0];
   if(id == isp::command::enterProgmode &&
      (avr8_.active() || !gateAllows(gate_) || (owner_ != nullptr && !owner_->acquire(client)))) {
      // The v2 interface owns the pins, debugWIRE uses them, or the gate refuses.
      out.u8(id);
      out.u8(isp::status::failed);
      return;
   }
   const std::size_t length = isp_.process(payload, out.free());
   if(length == 0) {
      out.failed(notSupported);
   } else {
      out.advance(length);
   }
   // The end of the programming mode gives the pins back. While the HID
   // processor is connected, its DAP_Disconnect does.
   if(id == isp::command::enterProgmode || id == isp::command::leaveProgmode) {
      releaseIdlePins();
   }
}

void EdbgTransport::avr8(art::span<const uint8_t> payload, Writer &out) {
   const uint8_t id = payload.empty() ? 0 : payload[0];
   const bool activate = id == avr8::command::activatePhysical && !avr8_.active();
   if(activate && (isp_.programming() || !gateAllows(gate_) ||
                   (owner_ != nullptr && !owner_->acquire(protocol::PinClient::CmsisDapHid)))) {
      // The v2 interface owns the pins, the ISP programming mode uses them, or the gate refuses.
      out.failed(avr8::failure::invalidConfig);
      return;
   }
   out.advance(avr8_.process(payload, out.free()));
   if(activate || id == avr8::command::deactivatePhysical || id == avr8::command::disableDebugWire) {
      releaseIdlePins();
   }
}

void EdbgTransport::edbgControl(art::span<const uint8_t> payload, Writer &out) {
   const bool set = !payload.empty() && payload[0] == cmdSet;
   const bool get = !payload.empty() && payload[0] == cmdGet;
   const auto access = set || get ? parameterAccess(payload) : std::nullopt;
   if(!access || access->context != edbgContextControl || access->address != edbgTargetPower || access->bytes != 1 ||
      power_ == nullptr || !power_->hardware().hasSwitch()) {
      out.failed(edbgNotSupported);
      return;
   }
   if(get) {
      const uint8_t on = power_->isOn() ? 1 : 0;
      out.data({ &on, 1 });
      return;
   }
   const uint8_t value = access->data[0];
   if(value == 0) {
      power_->switchOff();
      out.ok();
      return;
   }
   // Only the owner of the pins, or anyone while nobody owns them
   const bool mayDrive = owner_ == nullptr || owner_->isFreeFor(protocol::PinClient::CmsisDapHid);
   if(value != 1) {
      out.failed(edbgNotSupported);
   } else if(!mayDrive || (!power_->isOn() && power_->switchOn() != protocol::TargetPower::Result::Ok)) {
      // "On" is any supply: a 5 V supply stays at 5 V.
      out.failed(edbgIllegalAction);
   } else {
      out.ok();
   }
}

void EdbgTransport::releasePins() {
   // A frame that did not run yet must not take the pins again.
   if(framePending_) {
      dropFrame();
   }
   isp_.release();
   // The next host connection can be another tool on another chip: a device
   // context that stays would attach with its DWDR (issue #69).
   avr8_.forget();
}

void EdbgTransport::hostGone() {
   isp_.release();
   avr8_.forget();
   dropFrame();
   responseLength_ = 0;
   responseSent_ = 0;
}

} // namespace avr
