// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The EDBG transport of the AVR commands ("EDBG-based Tools Protocols",
// DS50002630A, chapters 3.2.1 and 5): the CMSIS-DAP vendor commands AVR_CMD,
// AVR_RSP and AVR_EVT on the HID interface, the JTAGICE3 frames inside them,
// and the scopes. Hardware-independent: the host unit tests run it through
// the HID function on a mock ISP target. The HID processor gives it the
// commands 0x80-0x82 (cmsisdap::Processor::setVendorCommands()).
//
// AVR_CMD [0x80][fragment m<<4|n][size u16 MSB first][data] -> [0x80][code]
//   carries fragment m of n of one command frame. Code 0x00: more
//   fragments to come, 0x01: the frame is complete. A fragment 1
//   starts a new frame: it drops a frame that is not complete and a
//   response that the host did not read. These fragments get code 0xFF and
//   drop the frame: m or n is 0, m > n, m is not the next fragment, n is not
//   the n of fragment 1, the size is more than the packet has, or the frame
//   is longer than maxFrameBytes.
// AVR_RSP [0x81] -> [0x81][fragment m<<4|n][size u16 MSB first][data]
//   gives the next fragment of the response, packet size - 4 bytes each.
//   Fragment 0x00 and size 0: no response.
// AVR_EVT [0x82] -> [0x82][size u16 MSB first][event frame]: the next
//   break event of the AVR8 scope (protocol/avr/Avr8Scope.h). Size 0: no
//   event.
//
// A command frame is [0x0E][version 0][sequence u16][handler][payload], the
// response frame [0x0E][sequence u16][handler][payload]. The sequence is
// LSB first, and the response has the sequence of the command. A frame
// shorter than 5 bytes, or with another SOF or version, gets no response.
// A complete frame runs in the main loop at the next vendor command
// (AVR_CMD, AVR_RSP or AVR_EVT), before that command: the AVR_CMD response
// goes to the host first. AVaRICE reads it for 200 ms only, and a GO with a
// software breakpoint can take 400 ms (issue #112). Limit: AVaRICE reads
// the AVR_RSP response for 500 ms, so a GO that writes two flash pages at
// 200 kHz is still too slow for it. A long command holds
// the HID interface as a long DAP command does. releasePins() and
// hostGone() drop a frame that did not run.
//
// The responses of the scopes, except AVR ISP, are OK [0x80][0], LIST
// [0x81][0][data], DATA [0x84][1][data][status 0] and FAILED
// [0xA0][0][code]. SET and GET are [cmd][0][context][address][bytes][data]:
// a range of parameters of one context.
//
//   0x00 Discovery     QUERY [0x00][0][context]: 0x00 the handlers, 0x80
//                      the tool name, 0x81 the serial number.
//   0x01 Housekeeping  START_SESSION (0x10), END_SESSION (0x11): OK. GET
//                      context 0: hardware version, firmware major, minor,
//                      build u16 (addresses 0-4); context 1 address 0: the
//                      target voltage in mV, u16 LSB first. SET: FAILED,
//                      the parameters are read-only. FW_UPGRADE (0x50) and
//                      the others: FAILED 0x10 (not supported).
//   0x11 AVR ISP       the STK500v2 commands (avr/IspScope.h).
//   0x12 AVR8 generic  debugWIRE and JTAG programming
//                      (protocol/avr/Avr8Scope.h). avrdude sets its
//                      parameters before ISP programming too.
//   0x20 EDBG control  SET and GET of context 0 address 0x10, TARGET_POWER
//                      (1 byte): 0 off, 1 on. FAILED 0x01 (not supported)
//                      on a board without a power switch, and for the other
//                      parameters and commands. A switch-on that the
//                      target power protection or the pin owner refuses:
//                      FAILED 0x16.
//   others             FAILED 0x10.
//
// The target pins (protocol/TargetPinOwner.h): the HID interface owns
// them. ENTER_PROGMODE and ACTIVATE_PHYSICAL take them: STATUS_CMD_FAILED
// or FAILED 0x33 while the v2 interface owns them, or while another AVR
// interface (ISP, debugWIRE or JTAG) is active. After a command that ends
// the ISP programming mode or the debugWIRE or JTAG session, the transport
// gives them back, unless the HID processor is connected (DAP_Connect):
// then its DAP_Disconnect gives them back. The target supply switches on
// only for the owner, or while nobody owns the pins; the switch-off always
// works.
//
// releasePins() (the HID processor gives the pins back: DAP_Disconnect, USB
// suspend, bus reset) ends the programming mode and the debugWIRE or JTAG
// session, and drops the AVR8 parameters and breakpoints. hostGone() also drops the
// frame and the response of the old host.

#include <array>
#include <cstddef>
#include <cstdint>

#include <utils/compat/span.h>

#include "protocol/TargetPinOwner.h"
#include "protocol/TargetPower.h"
#include "protocol/avr/Avr8Scope.h"
#include "protocol/avr/IspScope.h"
#include "protocol/cmsisdap/DapProcessor.h"

namespace avr {

namespace edbg {

// The CMSIS-DAP vendor commands
inline constexpr uint8_t avrCmd = 0x80;
inline constexpr uint8_t avrRsp = 0x81;
inline constexpr uint8_t avrEvt = 0x82;

// AVR_CMD: the codes of a fragment
inline constexpr uint8_t fragmentMore = 0x00;
inline constexpr uint8_t fragmentDone = 0x01;
inline constexpr uint8_t fragmentRejected = 0xFF;

inline constexpr uint8_t sof = 0x0E;

// The protocol handlers
namespace handler {
inline constexpr uint8_t discovery = 0x00;
inline constexpr uint8_t housekeeping = 0x01;
inline constexpr uint8_t avrIsp = 0x11;
inline constexpr uint8_t avr8 = 0x12;
inline constexpr uint8_t edbgControl = 0x20;
} // namespace handler

// The responses of the scopes
namespace response {
inline constexpr uint8_t ok = 0x80;
inline constexpr uint8_t list = 0x81;
inline constexpr uint8_t data = 0x84;
inline constexpr uint8_t failed = 0xA0;
} // namespace response

} // namespace edbg

// The fixed values of the tool
struct ToolInfo {
   // Discovery TOOL_NAME and SERIAL_NUMBER. nullptr is empty.
   const char *name{ nullptr };
   const char *serialNumber{ nullptr };
   uint8_t hardwareVersion{ 0 };
   uint8_t firmwareMajor{ 0 };
   uint8_t firmwareMinor{ 0 };
   uint16_t firmwareBuild{ 0 };
};

class EdbgTransport final : public cmsisdap::VendorCommands {
   public:
   // The longest command frame and response frame: an ISP read or program
   // of isp::maxDataBytes, or an AVR8 read or write of 256 bytes, with the
   // frame header, and room to spare.
   static constexpr std::size_t maxFrameBytes = 512;

   // dap: the HID processor, which owns the pins with the transport. power
   // and owner: nullptr for none. All must outlive the transport.
   EdbgTransport(const ToolInfo &info,
                 IspHandler &isp,
                 Avr8Handler &avr8,
                 const cmsisdap::Processor &dap,
                 protocol::TargetPower *power,
                 protocol::TargetPinOwner *owner);

   std::size_t command(uint8_t id, art::span<const uint8_t> fields, art::span<uint8_t> response) override;
   void releasePins() override;
   void hostGone() override;

   private:
   class Writer;

   std::size_t avrCommand(art::span<const uint8_t> fields, art::span<uint8_t> response);
   std::size_t avrResponse(art::span<uint8_t> response);
   std::size_t avrEvent(art::span<uint8_t> response);
   // Runs the complete command frame and stores its response frame.
   void runFrame();
   void discovery(art::span<const uint8_t> payload, Writer &out);
   void housekeeping(art::span<const uint8_t> payload, Writer &out);
   void avrIsp(art::span<const uint8_t> payload, Writer &out);
   void avr8(art::span<const uint8_t> payload, Writer &out);
   void edbgControl(art::span<const uint8_t> payload, Writer &out);
   // Drops the frame that is not complete.
   void dropFrame();
   // Gives the pins back when no AVR interface and no DAP_Connect uses them.
   void releaseIdlePins();

   const ToolInfo &info_;
   IspHandler &isp_;
   Avr8Handler &avr8_;
   const cmsisdap::Processor &dap_;
   protocol::TargetPower *power_;
   protocol::TargetPinOwner *owner_;

   std::array<uint8_t, maxFrameBytes> frame_{};
   std::size_t frameLength_{ 0 };
   // The fragment count of the frame, and the next fragment (0: none)
   uint8_t fragments_{ 0 };
   uint8_t nextFragment_{ 0 };
   // The frame is complete and runs at the next vendor command.
   bool framePending_{ false };

   std::array<uint8_t, maxFrameBytes> response_{};
   std::size_t responseLength_{ 0 };
   // The fragments of the response that the host has read
   uint8_t responseSent_{ 0 };
};

} // namespace avr
