// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The drive of the target pins: the order of the level shifter direction
// pins (board::Shifters) against the MCU pins of an engine, and the SRST pin
// with the polarity of DebugPins::srstActive. Hardware-independent: the
// host unit tests check it. board/BoardTargetDrive.h applies it to
// board::current on target.

#include <array>
#include <optional>

#include "board/BoardDefinition.h"
#include "protocol/GpioPins.h"

namespace board {

// The shifter groups that an engine drives. Out: TCK, TDI, TRST, AUX1 and
// AUX2 (DIR_OUT). OutAndSwdio: also TMS / SWDIO (DIR_SWDIO), for JTAG, where
// TMS is always an output.
enum class ShifterGroups : uint8_t { Out, OutAndSwdio };

// Drives the target outputs of an engine (on) or releases them (off).
// Without shifters, only setMcuPins() runs. With shifters:
// - on: first the pins of the DIR_OUT group get a defined output level,
//   because DIR_OUT high also drives the pins that the engine does not use:
//   TRST high (released), TDI high, AUX1 and AUX2 low. Then setMcuPins()
//   sets the pins of the engine, and then the direction pins of groups go
//   high.
// - off: both direction pins go low first (low is the safe state of each),
//   then setMcuPins() sets the pins of the engine to inputs, and then the
//   pins of the first step become inputs again.
// Thus a shifter does not drive the target from a floating MCU pin, and an
// MCU output does not drive against a shifter. Io has direction(pin, level)
// (write a direction pin), output(pin, level) and input(pin).
// Limit: on also releases a TRST that DAP_SWJ_Pins asserted before.
template <class Io, class SetMcuPins>
void driveTargetOutputs(const DebugPins &pins, const std::optional<Shifters> &shifters, ShifterGroups groups, bool on, Io &io, SetMcuPins &&setMcuPins) {
   if(!shifters) {
      setMcuPins();
      return;
   }
   struct Idle {
      Pin pin;
      bool level;
   };
   const std::array<std::optional<Idle>, 4> idle{ {
   pins.trst ? std::optional<Idle>(Idle{ *pins.trst, true }) : std::nullopt,
   Idle{ pins.tdi, true },
   Idle{ shifters->aux1, false },
   Idle{ shifters->aux2, false },
   } };
   if(on) {
      for(const auto &pin : idle) {
         if(pin) {
            io.output(pin->pin, pin->level);
         }
      }
      setMcuPins();
      io.direction(shifters->dirOut, true);
      if(groups == ShifterGroups::OutAndSwdio) {
         io.direction(shifters->dirSwdio, true);
      }
      return;
   }
   io.direction(shifters->dirOut, false);
   io.direction(shifters->dirSwdio, false);
   setMcuPins();
   for(const auto &pin : idle) {
      if(pin) {
         io.input(pin->pin);
      }
   }
}

// The setting of the SRST pin.
struct SrstPinSetting {
   protocol::PinMode mode;
   bool level;

   constexpr bool operator==(const SrstPinSetting &) const = default;
};

// The SRST pin setting for a request on the SRST gpio bit. The CMSIS-DAP
// processor and the Versaloon GPIO block see SRST as nRESET: open-drain and
// active low, a low level asserts the reset, an input releases it. With
// Active::Low, the pin is nRESET and the request stays. With Active::High,
// the pin drives a transistor that pulls nRESET low: the pin is a push-pull
// output with the inverse level, and a released line (an input request) is
// a low output, not an input with pull-up.
constexpr SrstPinSetting srstPinSetting(Active active, protocol::PinMode mode, bool level) {
   if(active == Active::Low) {
      return { mode, level };
   }
   const bool output = mode == protocol::PinMode::PushPull || mode == protocol::PinMode::OpenDrain;
   return { protocol::PinMode::PushPull, output && !level };
}

// The pin that reads nRESET. Active::Low: the SRST pin is nRESET. On the
// YD board the read-back input needs a wire that only debugWIRE uses, so
// it does not read SRST. Active::High: the read-back input when the board
// has it, else the SRST pin.
constexpr Pin srstReadPin(const DebugPins &pins) {
   return pins.srstActive == Active::High ? pins.srstSense.value_or(pins.srst) : pins.srst;
}

// The nRESET level from the level of srstReadPin(). Without a read-back
// input, the SRST pin of an Active::High board has the inverse level of
// nRESET.
constexpr bool srstLineLevel(const DebugPins &pins, bool pinLevel) {
   return srstReadPin(pins) == pins.srst && pins.srstActive == Active::High ? !pinLevel : pinLevel;
}

} // namespace board
