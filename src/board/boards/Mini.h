#pragma once

#include "board/BoardDefinition.h"

namespace board {

// Versaloon Mini (MiniRelease1): STM32F103C8 or STM32F103CB, 12 MHz crystal.
// The OtherUse has the same pins, except the USB pull-up (board/boards/OtherUse.h).
inline constexpr Definition mini{
   .name = "mini",
   .hseFrequency = 12'000'000,
   // MiniRelease1 in the original Versaloon firmware
   .hardwareId = 0x15,
   .debug{
   .tck{ Port::B, 13 },
   .tmsOut{ Port::B, 4 },
   .tmsIn{ Port::B, 6 },
   .tdi{ Port::B, 15 },
   .tdo{ Port::B, 14 },
   .trst = Pin{ Port::B, 10 },
   .srst{ Port::B, 11 },
   .rtck = Pin{ Port::A, 8 },
   },
   .jtagFastPath = JtagFastPath{ .tckLink{ Port::B, 3 } },
   .uart = Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } },
   .vref = VrefSense{ .pin{ Port::B, 0 }, .divider = 2 },
   .powerSwitch = PowerSwitch{ .pin{ Port::B, 8 }, .on = Active::Low },
   .leds{
   .power = Signal{ { Port::A, 15 }, Active::Low },
   .state = Signal{ { Port::B, 5 }, Active::Low },
   .usb = Signal{ { Port::A, 14 }, Active::Low },
   },
   .key = Signal{ { Port::B, 9 }, Active::Low },
   .usbPullUp = Signal{ { Port::C, 13 }, Active::High },
};

static_assert(isValid(mini));

} // namespace board
