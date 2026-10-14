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
//   0x01 Set area: [addr u32 BE][length u32 BE] -> [01]. The flash area
//        of the program sequence.
//   0x03 Memory read: [addr u32 BE][length u32 BE] -> [01]. Sets the range.
//   0x02 Program control: [sub] -> [sub]
//        0c The start of the memory read. The data then goes to the data
//           IN endpoint (dataPacket()).
//        01 Mass erase, with the own algorithm (WchLinkFlash.h)
//        05 Start of the program sequence: a reset that halts the hart.
//           Then the host sends the stub (512 bytes) on the data OUT
//           endpoint (submitData()).
//        07 Runs the stub to unlock the flash controller.
//        02 Start of the data phase: the image of the area on the data OUT
//           endpoint. After each block, the acknowledge [41 01 01 04] goes
//           to the data IN endpoint. A failed block gives [41 01 01 ff]
//           (not captured), and the module takes no more data.
//        08 End: locks the flash controller and restores the hart state.
//        Out of order, or after a failure: the error frame.
//   0x08 DMI access: [addr][data u32 BE][op] -> [addr][data u32 BE][status]
//        op 1: read, 2: write, 0: no access. Status 0: success, 2: failure.
//   0x06 Read protection:
//        01 Query -> [02] unprotected, [01] protected. Reads FLASH_OBR
//           (0x4002201c, RDPRT is bit 1) and 0x40022020 at the command, as
//           the WCH-LinkE does.
//        02 Unprotect -> [02], 03 Protect -> [03]: the option bytes, with
//           the own algorithm (WchLinkFlash.h).
//        04 -> [00], no target access.
//   0x0b Reset: [01] -> [01]. ndmreset: the hart restarts and runs.
//   0x0c Configure: [chip type][speed] -> [01].
//   0x0d Control:
//        01 Probe information -> [major][minor][type][00], the values of the
//           captured WCH-LinkE (2.19, type 0x12).
//        02 Connect -> [family][chip ID u32 BE]
//        03 -> [03], 04 -> [00], ff (quit) -> [ff]. Other values get the
//        unknown-command reply: the host tools use them for actions of the
//        probe (target power, erase, reset pin), see controlCommand().
//   0x0e Disable debug: [sub] -> [sub]. Not captured: as quit.
//   0x11 Chip information: [sub] -> 20 raw bytes without a header: the words
//        at 0x1ffff7e0, 0x1ffff7e8, 0x1ffff7ec and 0x1ffff7f0, and the chip
//        ID, each with the most significant byte first.
//
// - A packet that is not a request ([0x81][cmd][len]), or a command that is
//   not in the list, gets the reply of the WCH-LinkE to an unknown command:
//   [0x82][0x81][01][ff]. The command does nothing. This refuses the
//   other sub-commands of 0x02 and 0x06, and the WCH firmware-update and
//   mode-switch commands of the WCH-LinkE.
// - The payload length is not checked, as on the WCH-LinkE. A missing
//   payload byte reads 0.
// - Connect: the wire comes from the chip type of the last configure
//   command. 0x09 (CH32V00x) selects one wire. Other types try one wire
//   first, then two wires. The connect does the accesses of the probe
//   ("Probe actions"): DM registers 0x7e and 0x7d, dmstatus, halt, the chip
//   ID (0x7f), CSR 0x7c0. It fails with the error frame [0x81][0x55][01][01]
//   and the pins are released. A chip ID that is not in chipFamilies also
//   fails: a wrong family code would make the host use wrong chip settings.
//   The processor then resumes the hart, ends dmactive, and tries no other
//   wire.
// - Quit and disable debug resume the hart, write dmcontrol 40000000
//   (dmactive 0), and release the pins. Disable debug before a connect
//   only echoes.
// - A suspend (release()) and a lost host (hostGone()) release the pins
//   without target accesses: the hart stays halted until the next session.
// - A command that needs a target (DMI, reset, chip information, memory
//   read) before a connect, or a failed target access, gives the error
//   frame. A DMI access gives status 2 instead.
// - Memory read: each 32-bit word goes out with the most significant byte
//   first, in packets of packetSize bytes, until the length of the range.
//   After a failed word, the module stops reading and the rest of the data
//   is 0xff. A new command 0x03, a connect or a quit ends a read that the
//   host did not take completely. The transport cannot take back a packet
//   that is on its way to the host. Thus 02 0c gives the error frame while
//   a packet of the read before waits for the host (dataSent() not called
//   yet): its data would look like the start of the new read. A block
//   acknowledge also waits on the data IN endpoint.
// - Program sequence: a data packet runs before the next request
//   (runData()), so that a quit after the data finds the last block
//   programmed (minichlink sends no 02 08). A quit, a connect or an erase
//   ends the sequence as 02 08 does. A lost host drops it.
//
// The transport calls submit() for a request, when canSubmit() is true.
// runNext() runs it, in the main loop. One request and one response at a
// time.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/wchlink/WchLinkFlash.h"
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

// The sub-commands of command 0x02
inline constexpr uint8_t programErase = 0x01;
inline constexpr uint8_t programData = 0x02;
inline constexpr uint8_t programStub = 0x05;
inline constexpr uint8_t programUnlock = 0x07;
inline constexpr uint8_t programEnd = 0x08;
inline constexpr uint8_t programReadMemory = 0x0c;

// The block acknowledge on the data IN endpoint: 41 01 01 04. The failure
// value is not captured.
inline constexpr uint8_t ackHeader = 0x41;
inline constexpr uint8_t ackDone = 0x04;
inline constexpr uint8_t ackFailed = 0xff;

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

// The sub-commands of command 0x06
inline constexpr uint8_t protectionQuery = 0x01;
inline constexpr uint8_t protectionClear = 0x02;
inline constexpr uint8_t protectionSet = 0x03;
inline constexpr uint8_t protectionUnknown4 = 0x04;
inline constexpr uint8_t protectionUnknown4Reply = 0x00;
inline constexpr uint8_t protectionOff = 0x02;
inline constexpr uint8_t protectionOn = 0x01;
// FLASH_OBR and the register after it, read by the query
inline constexpr uint32_t flashObr = 0x4002'201c;
inline constexpr uint32_t flashWrpr = 0x4002'2020;
inline constexpr uint32_t obrRdprt = 1u << 1;

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

// A chip series: the top 12 bits of the chip ID, and the family code of the
// connect response
struct ChipFamily {
   uint16_t idPrefix;
   uint8_t family;
};

// The known chip series. Captured: CH32V003 (0x003, 0x09) and CH32V208
// (0x208, 0x05). The CH32V203 and the CH32V207 are in the CH32V20x series of
// the CH32V208. Their IDs are not captured.
inline constexpr std::array<ChipFamily, 4> chipFamilies{ {
{ 0x003, familyV00x },
{ 0x203, familyV20x },
{ 0x207, familyV20x },
{ 0x208, familyV20x },
} };

// The family code of the connect response. 0 for a chip ID that is not in
// chipFamilies.
constexpr uint8_t familyOf(uint32_t chipId) {
   for(const auto &entry : chipFamilies) {
      if(chipId >> 20 == entry.idPrefix) {
         return entry.family;
      }
   }
   return 0;
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

   // Memory read data or a block acknowledge waits for the data IN
   // endpoint.
   [[nodiscard]] bool hasData() const {
      return dataRemaining_ != 0 || ackPending_;
   }
   // The block acknowledge, or the next packet of the memory read from the
   // target. Valid until the next call. Empty when no data waits.
   art::span<const uint8_t> dataPacket();

   // The program sequence takes a packet of the data OUT endpoint, and none
   // waits.
   [[nodiscard]] bool wantsData() const;
   // Stores a packet of the data OUT endpoint for runData(). Needs
   // wantsData(). A packet of more than packetSize bytes is cut.
   void submitData(art::span<const uint8_t> packet);
   // Writes the stored data packet to the target. Returns false when none
   // is stored. Call it in the main loop, before runNext().
   bool runData();
   // The packet of dataPacket() went to the host, or the transport dropped
   // it.
   void dataSent() {
      dataInFlight_ = false;
   }

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
   // The result of a connect on one wire
   enum class Found : uint8_t {
      None,
      Known,
      // A target with a chip ID that is not in chipFamilies
      Unknown,
   };
   [[nodiscard]] Found connectOn(uint32_t &chipId);
   void quit();
   // Resumes the hart and ends dmactive, as the quit of the probe
   void restore();
   void configure(uint8_t chipType, uint8_t speed);
   void dmiCommand(art::span<const uint8_t> payload);
   void resetCommand(uint8_t sub);
   void chipInfoCommand();
   void protectionCommand(uint8_t sub);
   void setReadRange(uint32_t address, uint32_t length);
   void programCommand(uint8_t sub);
   void flashCommand(uint8_t sub);
   [[nodiscard]] const StubLayout &layout() const;
   [[nodiscard]] Series series() const;
   // Ends the program sequence (as 02 08). Returns false when it was not
   // complete, or a target access failed.
   bool endProgram();
   void queueAck(bool done);
   void detach();

   Target &target_;
   Platform &platform_;

   bool connected_{ false };
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
   // A packet of dataPacket() waits for dataSent().
   bool dataInFlight_{ false };
   std::array<uint8_t, packetSize> data_{};

   // The program sequence
   enum class Program : uint8_t {
      Idle,
      // 02 05: the stub arrives.
      Stub,
      // 02 07: the flash controller is unlocked.
      Unlocked,
      // 02 02: the image arrives.
      Data,
      // A step failed. 02 08 gives the error frame.
      Failed,
   };
   Flash flash_;
   Program program_{ Program::Idle };
   uint32_t areaAddress_{ 0 };
   uint32_t areaLength_{ 0 };
   std::array<uint8_t, packetSize> dataOut_{};
   std::size_t dataOutLength_{ 0 };
   bool hasDataOut_{ false };
   bool ackPending_{ false };
   uint8_t ackValue_{ ackDone };
};

} // namespace wchlink
