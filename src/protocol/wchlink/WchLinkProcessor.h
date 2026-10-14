#pragma once

// The WCH-Link command processor (RISC-V mode of the WCH-LinkE).
// Hardware-independent: it runs on a wchlink::Target and a Platform, and the
// host unit tests use the wire engine on mock pins. usb::WchLinkFunction
// moves the packets. The protocol comes from the captures of a WCH-LinkE
// (docs/wch-link-protocol.md).
//
// A request is one packet [0x81][cmd][len][payload]. The response is one
// packet [0x82][cmd][len][payload] on the command IN endpoint. Commands:
//
//   0x03 Memory read: [addr u32 BE][length u32 BE] -> [01]. Sets the range.
//   0x02 Program control: only 0x0c, the start of the memory read -> [0c].
//        The data then goes to the data IN endpoint (dataPacket()).
//   0x08 DMI access: [addr][data u32 BE][op] -> [addr][data u32 BE][status]
//        op 1: read, 2: write, 0: no access. Status 0: success, 2: failure.
//   0x0b Reset: [01] -> [01]. ndmreset: the hart restarts and runs.
//   0x0c Configure: [chip type][speed] -> [01].
//   0x0d Control:
//        01 Probe information -> [major][minor][type][00], the values of the
//           captured WCH-LinkE (2.19, type 0x12).
//        02 Connect -> [family][chip ID u32 BE]
//        03 -> [03], 04 -> [00], ff (quit) -> [ff], other values -> echo.
//   0x0e Disable debug: [sub] -> [sub]. Not captured: as quit.
//   0x11 Chip information: [sub] -> 20 raw bytes without a header: the words
//        at 0x1ffff7e0, 0x1ffff7e8, 0x1ffff7ec and 0x1ffff7f0, and the chip
//        ID, each with the most significant byte first.
//
// - A packet that is not a request ([0x81][cmd][len]), or a command that is
//   not in the list, gets the reply of the WCH-LinkE to an unknown command:
//   [0x82][0x81][01][ff]. The command does nothing. This refuses the flash
//   commands (0x01, 0x02 other than 0x0c, 0x06) until W5 adds them, and
//   the WCH firmware-update and mode-switch commands of the WCH-LinkE.
// - The payload length is not checked, as on the WCH-LinkE. A missing
//   payload byte reads 0.
// - Connect: the wire comes from the chip type of the last configure
//   command. 0x09 (CH32V00x) selects one wire. Other types try two wires
//   first, then one wire. The connect does the accesses of the probe
//   ("Probe actions"): DM registers 0x7e and 0x7d, dmstatus, halt, the chip
//   ID (0x7f), CSR 0x7c0. It fails with the error frame [0x81][0x55][01][01]
//   and the pins are released.
// - Quit and disable debug resume the hart, write dmcontrol 40000000
//   (dmactive 0), and release the pins.
// - A command that needs a target (DMI, reset, chip information, memory
//   read) before a connect, or a failed target access, gives the error
//   frame. A DMI access gives status 2 instead.
// - Memory read: each 32-bit word goes out with the most significant byte
//   first, in packets of packetSize bytes, until the length of the range.
//   After a failed word, the module stops reading and the rest of the data
//   is 0xff. A new command 0x03, a connect or a quit ends a read that the
//   host did not take completely.
//
// The transport calls submit() for a request, when canSubmit() is true.
// runNext() runs it, in the main loop. One request and one response at a
// time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/wchlink/WchLinkTarget.h"

namespace wchlink {

namespace command {
inline constexpr uint8_t setArea = 0x01;
inline constexpr uint8_t program = 0x02;
inline constexpr uint8_t readMemory = 0x03;
inline constexpr uint8_t protection = 0x06;
inline constexpr uint8_t dmi = 0x08;
inline constexpr uint8_t reset = 0x0b;
inline constexpr uint8_t configure = 0x0c;
inline constexpr uint8_t control = 0x0d;
inline constexpr uint8_t disableDebug = 0x0e;
inline constexpr uint8_t chipInfo = 0x11;
} // namespace command

// The sub-commands of command 0x0d
namespace control {
inline constexpr uint8_t info = 0x01;
inline constexpr uint8_t connect = 0x02;
inline constexpr uint8_t quit = 0xff;
} // namespace control

// The sub-command of command 0x02 that starts the memory read
inline constexpr uint8_t programReadMemory = 0x0c;

inline constexpr uint8_t requestHeader = 0x81;
inline constexpr uint8_t responseHeader = 0x82;
inline constexpr uint8_t errorHeader = 0x81;
// The reason of the error frame of a failed connect
inline constexpr uint8_t errorNoTarget = 0x55;

// The DMI status of command 0x08
inline constexpr uint8_t dmiOk = 0x00;
inline constexpr uint8_t dmiFailed = 0x02;
inline constexpr uint8_t dmiNop = 0x00;
inline constexpr uint8_t dmiRead = 0x01;
inline constexpr uint8_t dmiWrite = 0x02;

// The chip type of command 0x0c for the CH32V00x, and the family code of
// the connect response. Captured: 0x09 (CH32V003), 0x05 (CH32V208).
inline constexpr uint8_t familyV00x = 0x09;
inline constexpr uint8_t familyV20x = 0x05;
inline constexpr uint8_t familyV30x = 0x06;

// The probe information of the captured WCH-LinkE: firmware 2.19, probe
// type 0x12 (wlink: WCH-LinkE-CH32V305)
inline constexpr uint8_t firmwareMajor = 2;
inline constexpr uint8_t firmwareMinor = 19;
inline constexpr uint8_t probeType = 0x12;

// The registers of the probe actions at connect
namespace wchdm {
inline constexpr uint8_t unlock1 = 0x7e;
inline constexpr uint8_t unlock2 = 0x7d;
inline constexpr uint32_t unlockValue = 0x5aa5'0400;
inline constexpr uint16_t csr7c0 = 0x07c0;
inline constexpr uint32_t csr7c0Value = 0x0000'0300;
} // namespace wchdm

// The words of the chip information (command 0x11)
inline constexpr std::array<uint32_t, 4> chipInfoAddresses{ 0x1fff'f7e0, 0x1fff'f7e8, 0x1fff'f7ec, 0x1fff'f7f0 };
inline constexpr std::size_t chipInfoSize = 20;

// The family code of the connect response, from the wire and the chip ID.
// Only 0x09 and 0x05 are captured. 0x06 for the CH32V30x is not.
constexpr uint8_t familyOf(rvswd::Wire wire, uint32_t chipId) {
   if(wire == rvswd::Wire::One) {
      return familyV00x;
   }
   return (chipId >> 24) == 0x30 ? familyV30x : familyV20x;
}

// Time of the board
class Platform {
   public:
   virtual void delayUs(uint32_t microseconds) = 0;

   protected:
   ~Platform() = default;
};

class Processor {
   public:
   static constexpr std::size_t packetSize = 64;
   // The time that reset holds ndmreset
   static constexpr uint32_t resetPulseUs = 1'000;

   Processor(Target &target, Platform &platform);

   // No request waits and no response is unsent.
   [[nodiscard]] bool canSubmit() const {
      return !hasRequest_ && responseLength_ == 0;
   }

   // Stores a request for runNext(). Needs canSubmit(). A request of more
   // than packetSize bytes is cut.
   void submit(art::span<const uint8_t> request);

   // Runs the stored request. Returns false when none is stored. Call it in
   // the main loop.
   bool runNext();

   [[nodiscard]] bool hasResponse() const {
      return responseLength_ != 0;
   }
   // The response, valid until popResponse().
   [[nodiscard]] art::span<const uint8_t> response() const {
      return { response_.data(), responseLength_ };
   }
   void popResponse() {
      responseLength_ = 0;
   }

   // Memory read data waits for the data IN endpoint.
   [[nodiscard]] bool hasData() const {
      return dataRemaining_ != 0;
   }
   // Reads the next packet of the memory read from the target. Valid until
   // the next call. Empty when no data waits.
   art::span<const uint8_t> dataPacket();

   // The host went away: like quit without the target accesses, and the
   // request, the response and the memory read are dropped.
   void hostGone();

   // Releases the pins (a suspend). The request and the response stay. The
   // host must connect again.
   void release();

   [[nodiscard]] bool connected() const {
      return connected_;
   }

   private:
   void execute(art::span<const uint8_t> request);
   void reply(uint8_t command, art::span<const uint8_t> payload);
   void replyByte(uint8_t command, uint8_t value);
   void replyError();
   void replyUnknown();

   void controlCommand(uint8_t sub);
   [[nodiscard]] bool connect();
   [[nodiscard]] bool connectOn(uint32_t &chipId);
   void quit();
   void configure(uint8_t chipType, uint8_t speed);
   void dmiCommand(art::span<const uint8_t> payload);
   void resetCommand(uint8_t sub);
   void chipInfoCommand();
   void setReadRange(uint32_t address, uint32_t length);
   void programCommand(uint8_t sub);
   void detach();

   Target &target_;
   Platform &platform_;

   bool connected_{ false };
   rvswd::Wire wire_{ rvswd::Wire::Two };
   uint32_t chipId_{ 0 };
   // The chip type of the last configure command
   uint8_t chipType_{ 0 };

   std::array<uint8_t, packetSize> request_{};
   std::size_t requestLength_{ 0 };
   bool hasRequest_{ false };
   std::array<uint8_t, packetSize> response_{};
   std::size_t responseLength_{ 0 };

   // The range of command 0x03, and the memory read in progress
   uint32_t readAddress_{ 0 };
   uint32_t readLength_{ 0 };
   uint32_t dataAddress_{ 0 };
   uint32_t dataRemaining_{ 0 };
   bool dataFailed_{ false };
   std::array<uint8_t, packetSize> data_{};
};

} // namespace wchlink
