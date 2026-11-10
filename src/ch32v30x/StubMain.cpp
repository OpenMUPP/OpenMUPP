// SPDX-License-Identifier: GPL-3.0-or-later

// Image of the CH32V307 boards (cmake/Ch32StubImage.cmake) until the port is
// complete. os_startup_initialize_hardware() (initialize_hardware.cpp) set the
// clock, the 1 ms SysTick and the board pins before main(). main() flashes the
// state LED: a short flash once per second, or the fast clock-fault pattern
// when the crystal did not start. USB and the probe engines are not in it yet.

#include <cmsis_device.h>
#include <utils/Timekeeping.h>

#include "Hardware.h"
#include "Heartbeat.h"
#include "board/Leds.h"

int main() {
   constexpr Heartbeat running{ Heartbeat::Pattern{ .periodMs = 1000, .onMs = 50 } };
   constexpr Heartbeat clockFault{ Heartbeat::clockFault };
   const bool clockFromCrystal = hardware::isClockFromCrystal();
   const board::StateShow flash = clockFromCrystal ? board::StateShow::Activity : board::StateShow::Fault;
   const Heartbeat &heartbeat = clockFromCrystal ? running : clockFault;
   for(;;) {
      const auto nowMs = static_cast<uint64_t>(getUptimeTicks());
      board::showState(heartbeat.isOn(nowMs) ? flash : board::StateShow::Idle);
      // The SysTick ends the WFI
      __WFI();
   }
}
