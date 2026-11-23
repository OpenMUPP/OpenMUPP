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
struct BoardTargetIo {
   static void direction(Pin pin, bool level) {
      gpioPin(pin).write(level);
   }
   static void output(Pin pin, bool level) {
      const hw::GpioPin gpio = gpioPin(pin);
      gpio.write(level);
      gpio.configure(hw::GpioPin::Mode::Output);
   }
   static void input(Pin pin) {
      gpioPin(pin).configure(hw::GpioPin::Mode::Input);
   }
};

// The users of DIR_OUT: the engines and BoardGpio
inline DirOutUsers dirOutUsers;

template <class SetMcuPins>
void driveTargetOutputs(bool on, SetMcuPins &&setMcuPins, ShifterGroups groups = ShifterGroups::Out) {
   BoardTargetIo io;
   driveTargetOutputs(current.debug, current.shifters, dirOutUsers, groups, on, io, setMcuPins);
}

} // namespace board
