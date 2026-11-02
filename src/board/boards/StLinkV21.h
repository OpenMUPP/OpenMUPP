// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board/BoardDefinition.h"

namespace board {

// ST-Link/V2-1 of the Nucleo-64 boards (STM32F103CBT6, 8 MHz crystal). The
// pins come from the schematics MB1136 (sheet 4) and MB1360 (sheet 7): the
// two boards use the same MCU pins. Not tested on hardware.
//
// The board ties PA5 + PB13 (T_JTCK) and PB14 + PB12 (SWDIO, PB12 through
// 100 Ohm). As on the mini clone, PB14 drives SWDIO and PB12 reads it.
// The Nucleo has an SWD connector only (CN4): TDI (PA7) and TDO (PA6) are
// not connected, and T_JRST (PB1) has no connector pin. SWO (PA10) is on
// CN4 pin 6, but the firmware does not use it.
// No JTAG fast path, no power switch and no key. USB_RENUMn (PA15) drives
// an NPN emitter follower to the D+ pull-up: high connects, low
// disconnects.
inline constexpr Definition stLinkV21{
   .name = "stlinkv21",
   .hseFrequency = 8'000'000,
   .family = Family::Stm32F1,
   // ST_Link in the original Versaloon firmware
   .hardwareId = 0x38,
   .debug{
   .tck{ Port::A, 5 },
   .tmsOut{ Port::B, 14 },
   .tmsIn{ Port::B, 12 },
   .tdi{ Port::A, 7 },
   .tdo{ Port::A, 6 },
   .trst = std::nullopt,
   .srst{ Port::B, 0 },
   .rtck = std::nullopt,
   },
   .jtagFastPath = std::nullopt,
   .uart = Uart{ .usart = 2, .tx{ Port::A, 2 }, .rx{ Port::A, 3 } },
   // Divider 2 on PA0 from the 3.3 V of the Nucleo, not from the target:
   // the resistor from CN4 pin 1 (VDD_TARGET) is not fitted. A board that is
   // broken off the Nucleo reads 0 V.
   .vref = VrefSense{ .pin{ Port::A, 0 }, .divider = 2 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   // Bicolor LED on PA9: high lights red, low lights green. Idle is green,
   // activity switches both off, a fault is red.
   .state = Signal{ { Port::A, 9 }, Active::Low },
   .usb = std::nullopt,
   .stateTwoColour = true,
   },
   .key = std::nullopt,
   .usbPullUp = Signal{ { Port::A, 15 }, Active::High },
};
static_assert(isValid(stLinkV21));

} // namespace board
