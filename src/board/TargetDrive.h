// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The drive of the target pins: the order of the level shifter direction
// pins (board::Shifters) against the MCU pins of an engine, and the SRST pin
// with the polarity of DebugPins::srstActive. Hardware-independent: the
// host unit tests check it. board/BoardTargetDrive.h applies it to
// board::current on target.

#include <optional>

#include "board/BoardDefinition.h"
#include "protocol/GpioPins.h"

namespace board {

// The shifter groups that an engine drives. Out: TCK, TDI, TRST, AUX1 and
// AUX2 (DIR_OUT). OutAndSwdio: also TMS / SWDIO (DIR_SWDIO), for JTAG, where
// TMS is always an output.
enum class ShifterGroups : uint8_t { Out, OutAndSwdio };

// Drives the target outputs of an engine (on) or releases them (off).
// on: setMcuPins() sets the MCU pins first, then the direction pins go high.
// off: the direction pins go low first, then setMcuPins() sets the MCU pins
// to inputs. Thus a shifter never drives the target from a floating MCU
// pin, and an MCU output never drives against a shifter. Without shifters,
// only setMcuPins() runs. setDirection(pin, level) writes a direction pin.
template <class SetDirection, class SetMcuPins>
void driveTargetOutputs(const std::optional<Shifters> &shifters, ShifterGroups groups, bool on, SetDirection &&setDirection, SetMcuPins &&setMcuPins) {
   if(on) {
      setMcuPins();
   }
   if(shifters) {
      setDirection(shifters->dirOut, on);
      if(groups == ShifterGroups::OutAndSwdio) {
         setDirection(shifters->dirSwdio, on);
      }
   }
   if(!on) {
      setMcuPins();
   }
}

} // namespace board
