#pragma once

// The emulated TAP of the CMSIS-DAP RISC-V bridge: a JTAG port
// (protocol/JtagPort.h) with one RISC-V Debug Transport Module (DTM, debug
// spec 0.13, chapter 6.1) and no pins. The JTAG-DP (DapJtag.h) sends
// DAP_JTAG_Sequence and DAP_SWJ_Sequence to this port as to a real one. The
// TAP follows the TMS and TDI bits and gives the TDO bits.
// Hardware-independent: the host unit tests run it.
//
// - IR: 5 bits. Capture-IR loads 00001. Test-Logic-Reset selects IDCODE.
// - IDCODE (0x01): rvDtm::idcode.
// - DTMCS (0x10): rvDtm::dtmcs. A write has no effect.
// - All other instructions select BYPASS (1 bit, Capture-DR loads 0). The
//   DMI register (0x11) comes with the DMI access on the wire (#106).
//
// TDO is the bit 0 of the shift register in Shift-DR and Shift-IR, and high
// in the other states, as an open TDO line with a pull-up.

#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/JtagPort.h"

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

   State state_{ State::Reset };
   uint8_t ir_{ rvDtm::irIdcode };
   // The shift register of Shift-IR or Shift-DR, and its length
   uint32_t shift_{ 0 };
   uint8_t length_{ 0 };
};

} // namespace cmsisdap
