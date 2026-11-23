// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// board::driveTargetOutputs() (board/TargetDrive.h) on the shifters of
// board::current. Target only.

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/TargetDrive.h"

namespace board {

// The engines call this in init() with the code that makes their MCU pins
// outputs, and in fini() with the code that makes them inputs. The direction
// pins are outputs from the start-up (initialize_hardware.cpp).
template <class SetMcuPins>
void driveTargetOutputs(bool on, SetMcuPins &&setMcuPins, ShifterGroups groups = ShifterGroups::Out) {
   driveTargetOutputs(current.shifters, groups, on, [](Pin pin, bool level) { gpioPin(pin).write(level); }, setMcuPins);
}

} // namespace board
