// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// board::driveTargetOutputs() (board/TargetDrive.h) on the shifters of
// board::current. Target only.

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"
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
   static void write(Pin pin, bool level) {
      gpioPin(pin).write(level);
   }
   static bool read(Pin pin) {
      return gpioPin(pin).read();
   }
   // About 2 us: the round trip through the shifters is about 34 ns at
   // 1.8 V, and the target line has its capacitance.
   static void settle() {
      startCycleCounter();
      const uint32_t start = cycleCount();
      while(cycleCount() - start < SystemCoreClock / 500'000) {
      }
   }
};

// The users of DIR_OUT: the engines and BoardGpio
inline DirOutUsers dirOutUsers;
// The failed connect self-tests (connectSelfTest()). It counts as a probe
// error (main.cpp, error LED). The engine still runs: its transfers then
// fail, and the host sees the error.
inline uint32_t selfTestErrors;

template <class SetMcuPins>
void driveTargetOutputs(bool on, SetMcuPins &&setMcuPins, ShifterGroups groups = ShifterGroups::Out) {
   BoardTargetIo io;
   if(!driveTargetOutputs(current.debug, current.shifters, dirOutUsers, groups, on, io, setMcuPins)) {
      ++selfTestErrors;
   }
}

} // namespace board
