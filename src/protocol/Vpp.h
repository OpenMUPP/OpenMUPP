// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The VPP driver: the high voltage on the target pin of a high-voltage
// adapter (OpenMUPP v1: J4 pin 19). Hardware-independent: it runs on a
// VppHardware object, and the host unit tests use a fake one. The sequence
// and the limits are in docs/boards.md ("VPP sequence").
//
// switchOn() sets a level from minMillivolts to hardwareMaxMillivolts. It
// refuses a level outside this range, above the limit of the target family
// (the caller gives it), and above the limit of the gate. The gate reads the
// adapter ID again at each switch-on (VppGate::check()). A refusal of the
// gate also switches VPP off.
//
// The change sequence:
//
//   1. VPP_PD high, then VPP_ON low: on a level change, the target pin
//      goes from VPP to 0 V, never to the run level.
//   2. The DAC to the level (vppDacCode()).
//   3. VPP_EN high. Each correctIntervalMs, read VPP_SENSE. The resistor
//      and reference tolerances make the DAC value inexact, so the driver
//      corrects the DAC by half of the error, but only while the output is
//      steady: in a step down only the dividers discharge the capacitors
//      (about 0.7 s from 14 V to 5 V). The level is there when VPP_SENSE
//      is steady and in the window (level +- toleranceMillivolts, never
//      above the limit) for settleReads reads. When this does not occur in
//      levelTimeoutMs, VPP goes off and switchOn() replies NoLevel.
//   4. VPP_ON high (VPP_PD is still high), wait switchUs, VPP_PD low: the
//      target pin goes from 0 V to VPP with no step at the run level.
//   5. After sense2SettleMs, VPP_SENSE2 (the target pin) must be at most
//      maxDropMillivolts below VPP_SENSE. Else the pin has a short or an
//      overload (the current limit of the output switch), VPP goes off and
//      switchOn() replies TargetFault.
//
// switchOff(): VPP_PD high, then VPP_ON low and VPP_EN low (the program
// exit of docs/hardware.md of openmupp-v1): the target pin goes from VPP
// to 0 V directly and stays there. The DAC goes off.
//
// poll() switches VPP off when the gate does not allow it any more, for
// example after a refused connect. While VPP is on, it also reads both
// senses each correctIntervalMs, and switches VPP off when a read fails,
// when VPP_SENSE2 is more than maxDropMillivolts below VPP_SENSE (a short
// or an overload on the target pin), or when VPP_SENSE is more than
// toleranceMillivolts above the limit of the switch-on.

#include <cstdint>
#include <optional>

#include "protocol/Delay.h"
#include "protocol/VppHardware.h"

namespace protocol {

// The VPP rule of the board (board/BoardIdSense.h)
class VppGate {
   public:
   // A switch-on: reads the adapter ID again. The highest VPP level in mV
   // that the adapter allows, no value when VPP is not allowed.
   [[nodiscard]] virtual std::optional<uint16_t> check() = 0;
   // The result of the last check(), without a new reading
   [[nodiscard]] virtual bool isAllowed() const = 0;

   protected:
   ~VppGate() = default;
};

// VPP = 14.1 V - 4.5 x V_DAC: the feedback divider R34 (180 kOhm), R35
// (10 kOhm) and the DAC injection R57 + R58 (40 kOhm) of OpenMUPP v1 with
// the typical 0.6 V reference of the MT3608 (docs/hardware.md of the
// openmupp-v1 repository). The DAC reference is 3.3 V.
inline constexpr uint16_t vppAtDacZeroMillivolts = 14100;

// DAC codes for one mV of VPP: V_DAC = (14100 mV - VPP) / 4.5, and the
// code is V_DAC * 4095 / 3300.
inline constexpr uint32_t vppDacPerMvNum = 2 * 4095;
inline constexpr uint32_t vppDacPerMvDen = 9 * 3300;

// The DAC code for a level in mV: 0 at 14.1 V and above
constexpr uint16_t vppDacCode(uint16_t millivolts) {
   if(millivolts >= vppAtDacZeroMillivolts) {
      return 0;
   }
   return static_cast<uint16_t>(((vppAtDacZeroMillivolts - millivolts) * vppDacPerMvNum + vppDacPerMvDen / 2) / vppDacPerMvDen);
}

static_assert(vppDacCode(14100) == 0);
static_assert(vppDacCode(14700) == 0);
// 11.4 V: V_DAC = 0.6 V, the level of the DAC off
static_assert(vppDacCode(11400) == 745);
static_assert(vppDacCode(5000) == 2509);
// 0 V: 3.13 V on the DAC, in its range
static_assert(vppDacCode(0) == 3888);

class Vpp {
   public:
   // The lowest level: the boost passes VBUS through below it
   static constexpr uint16_t minMillivolts = 5000;
   // The hardware maximum with the worst-case tolerances (docs/boards.md)
   static constexpr uint16_t hardwareMaxMillivolts = 14700;
   // The window of VPP_SENSE around the level. The ADC step is about 4 mV.
   static constexpr uint16_t toleranceMillivolts = 100;
   // The longest wait for the level (docs/boards.md)
   static constexpr uint16_t levelTimeoutMs = 2000;
   // Time between two corrections of the DAC
   static constexpr uint16_t correctIntervalMs = 10;
   // A change of at most this in correctIntervalMs is a steady output
   static constexpr uint16_t steadyMillivolts = 20;
   // Consecutive steady reads in the window that end the wait
   static constexpr unsigned settleReads = 3;
   // The overlap of VPP_PD and VPP_ON at the switch-on: the turn-on time
   // of the output switch Q2. An estimate (200 us), measure it on the
   // first board.
   static constexpr uint16_t switchUs = 200;
   // The filter of VPP_SENSE2 (C33 and the divider) is about 2 ms
   static constexpr uint16_t sense2SettleMs = 10;
   // The highest drop from VPP_SENSE to VPP_SENSE2 with VPP on: the
   // divider tolerances (0.3 V at 14 V) and some mA in R52 (33 mV/mA).
   // More is a short or an overload on the target pin.
   static constexpr uint16_t maxDropMillivolts = 500;

   enum class Result : uint8_t {
      Ok,
      // The level is out of range or above a limit, or the gate refused
      Refused,
      // VPP_SENSE did not show the level in levelTimeoutMs, or the
      // measurement failed
      NoLevel,
      // VPP_SENSE2 shows a short or an overload on the target pin
      TargetFault,
   };

   Vpp(VppHardware &hardware, Delay &delay, VppGate &gate);

   // Switches VPP on at `millivolts`, or changes the level. familyLimit:
   // the highest VPP of the target family. VPP is off after a result that
   // is not Ok, except after a refused level: then VPP stays as it was.
   [[nodiscard]] Result switchOn(uint16_t millivolts, uint16_t familyLimit = hardwareMaxMillivolts);
   void switchOff();

   // Call it in the main loop. nowMs is a millisecond counter. True when
   // the call switched VPP off.
   [[nodiscard]] bool poll(uint64_t nowMs);

   [[nodiscard]] bool isOn() const {
      return level_ != 0;
   }

   // The level in mV, 0 when VPP is off
   [[nodiscard]] uint16_t level() const {
      return level_;
   }

   [[nodiscard]] VppHardware &hardware() {
      return hardware_;
   }

   private:
   // Steps 2 and 3 of the sequence: true when VPP_SENSE shows the level
   [[nodiscard]] bool reachLevel(uint16_t millivolts, uint16_t limit);

   VppHardware &hardware_;
   Delay &delay_;
   VppGate &gate_;
   uint16_t level_{ 0 };
   // The limit of the last switch-on, for the check in poll()
   uint16_t limit_{ 0 };
   uint64_t lastCheckMs_{ 0 };
};

} // namespace protocol
