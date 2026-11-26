// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol/cmsisdap/DapVpp.h"

namespace cmsisdap {

namespace {

constexpr uint8_t vendor5 = 0x85;
constexpr uint8_t opOff = 0;
constexpr uint8_t opOn = 1;
constexpr uint8_t opRead = 2;
constexpr uint8_t statusOk = 0;
constexpr uint8_t statusRefused = 1;
constexpr uint8_t statusNoLevel = 2;
constexpr uint8_t statusTargetFault = 3;
constexpr uint8_t statusError = 0xFF;
constexpr std::size_t responseLength = 8;

uint8_t statusOf(protocol::Vpp::Result result) {
   switch(result) {
   case protocol::Vpp::Result::Ok:
      return statusOk;
   case protocol::Vpp::Result::Refused:
      return statusRefused;
   case protocol::Vpp::Result::NoLevel:
      return statusNoLevel;
   case protocol::Vpp::Result::TargetFault:
      return statusTargetFault;
   }
   return statusError;
}

void put16(art::span<uint8_t> out, std::size_t offset, uint16_t value) {
   out[offset] = static_cast<uint8_t>(value);
   out[offset + 1] = static_cast<uint8_t>(value >> 8);
}

} // namespace

std::size_t DapVpp::command(art::span<const uint8_t> fields, art::span<uint8_t> response, bool pinsFree) {
   // The room before the switch: a response that does not fit changes
   // nothing.
   if(fields.empty() || response.size() < responseLength) {
      return 0;
   }
   const uint8_t op = fields[0];
   if(op == opOn && fields.size() < 3) {
      return 0;
   }
   uint8_t status = statusOk;
   if(op == opOn && !pinsFree) {
      status = statusRefused;
   } else if(op == opOn) {
      status = statusOf(vpp_.switchOn(static_cast<uint16_t>(fields[1] | (fields[2] << 8))));
   } else if(op == opOff) {
      vpp_.switchOff();
   } else if(op != opRead) {
      status = statusError;
   }
   response[0] = vendor5;
   response[1] = status;
   put16(response, 2, vpp_.level());
   put16(response, 4, vpp_.hardware().senseMillivolts().value_or(0));
   put16(response, 6, vpp_.hardware().sense2Millivolts().value_or(0));
   return responseLength;
}

} // namespace cmsisdap
