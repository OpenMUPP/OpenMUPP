#pragma once

// The emulated TAP of the CMSIS-DAP RISC-V bridge: a JTAG port
// (protocol/JtagPort.h) with one RISC-V Debug Transport Module (DTM, debug
// spec 0.13, chapter 6.1) and no pins. The JTAG-DP (DapJtag.h) sends
// DAP_JTAG_Sequence and DAP_SWJ_Sequence to this port as to a real one. The
// TAP follows the TMS and TDI bits and gives the TDO bits.
// Hardware-independent: the DMI accesses go to a wchlink::Target
// (protocol/wchlink/WchLinkTarget.h), and the host unit tests run it on mock
// pins with a mock DM.
//
// - IR: 5 bits. Capture-IR loads 00001.
// - IDCODE (0x01): rvDtm::idcode.
// - DTMCS (0x10): rvDtm::dtmcs, with dmistat 2 while the DMI error is set.
//   A write of dmireset clears the error. A write of dmihardreset also
//   clears the result of the last access.
// - DMI (0x11): 41 bits, address[40:34], data[33:2], op[1:0].
// - All other instructions select BYPASS (1 bit, Capture-DR loads 0).
// - Test-Logic-Reset selects IDCODE, and resets the DTM as dmihardreset.
//
// The DMI access (debug spec 0.13, 6.1.5): Update-DR with op 1 (read) or 2
// (write) does the access on the wire at once. The next Capture-DR loads
// the address, the read data and op 0 (success) or 2 (failed). A failed
// access sets the sticky error. While it is set, Update-DR starts no
// access. op 3 (busy) never occurs: the access ends before the next scan.
// A write and a failed read keep the data of the last read.
//
// Limit: mainline OpenOCD checks op 2 only in its batch memory read, and
// sends dmireset only after op 3. A failed single access then looks like a
// success, and the sticky error blocks all accesses until Test-Logic-Reset.
// The error model is an open question of #103 (open question 4).
//
// TDO is the bit 0 of the shift register in Shift-DR and Shift-IR, and high
// in the other states, as an open TDO line with a pull-up.

#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/JtagPort.h"
#include "protocol/wchlink/WchLinkTarget.h"

namespace cmsisdap {

namespace rvDtm {
inline constexpr uint8_t irLength = 5;
inline constexpr uint8_t irCapture = 0x01;

// The instructions
inline constexpr uint8_t irIdcode = 0x01;
inline constexpr uint8_t irDtmcs = 0x10;
inline constexpr uint8_t irDmi = 0x11;
inline constexpr uint8_t irBypass = 0x1F;

// Provisional (#103, open question 1). Manufacturer ID 0 is not a valid
// JEDEC ID, so no config of a real part expects this value.
inline constexpr uint32_t idcode = 0x15653001;

// DTMCS fields
inline constexpr uint32_t dtmcsVersion013 = 1;
inline constexpr uint32_t abits = 7;
// The DMI access is complete before the next scan, so no idle cycles.
inline constexpr uint32_t idleCycles = 0;
inline constexpr uint32_t dtmcs = dtmcsVersion013 | (abits << 4) | (idleCycles << 12);
inline constexpr uint32_t dmistatShift = 10;
inline constexpr uint32_t dmireset = 1u << 16;
inline constexpr uint32_t dmihardreset = 1u << 17;

// The DMI register: op in bits 1:0, data in bits 33:2, the address above
inline constexpr uint8_t dmiLength = 34 + abits;
inline constexpr uint8_t dataShift = 2;
inline constexpr uint8_t addressShift = 34;
// op: the request at Update-DR, the status at Capture-DR
inline constexpr uint32_t opNop = 0;
inline constexpr uint32_t opRead = 1;
inline constexpr uint32_t opWrite = 2;
inline constexpr uint32_t opFailed = 2;
} // namespace rvDtm

class RvDtmTap final : public protocol::JtagPort {
   public:
   enum class State : uint8_t {
      Reset,
      Idle,
      SelectDr,
      CaptureDr,
      ShiftDr,
      Exit1Dr,
      PauseDr,
      Exit2Dr,
      UpdateDr,
      SelectIr,
      CaptureIr,
      ShiftIr,
      Exit1Ir,
      PauseIr,
      Exit2Ir,
      UpdateIr,
   };

   explicit RvDtmTap(wchlink::Target &target)
      : target_(target) {
   }

   void init() override {
   }
   void fini() override {
   }
   [[nodiscard]] bool setSpeed(uint32_t) override {
      return true;
   }
   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override;

   [[nodiscard]] State state() const {
      return state_;
   }

   [[nodiscard]] uint8_t ir() const {
      return ir_;
   }

   private:
   // One TCK cycle. Returns TDO.
   bool clock(bool tms, bool tdi);
   void captureDr();
   void updateDr();
   void dmiAccess();
   // dmihardreset
   void hardReset();

   wchlink::Target &target_;
   State state_{ State::Reset };
   uint8_t ir_{ rvDtm::irIdcode };
   // The shift register of Shift-IR or Shift-DR, and its length
   uint64_t shift_{ 0 };
   uint8_t length_{ 0 };
   // The DMI result for the next Capture-DR, and the sticky error
   uint8_t address_{ 0 };
   uint32_t data_{ 0 };
   bool error_{ false };
};

} // namespace cmsisdap
