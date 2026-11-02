// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board/BoardDefinition.h"

namespace board {

// ST-Link/V2-1 of the Nucleo-64 boards (STM32F103CBT6, 8 MHz crystal). The
// pins come from the schematics MB1136 (sheet 4) and MB1360 (sheet 7): the
// two boards use the same MCU pins. Tested on hardware: WCH-Link on one
// wire (CH32V006, MB1360). A one-wire target needs an external SWDIO
// pull-up (docs/boards.md).
//
// The board ties PA5 + PB13 (T_JTCK) and PB14 + PB12 (SWDIO, PB12 through
// 100 Ohm). As on the mini clone, PB14 drives SWDIO and PB12 reads it.
// The Nucleo has an SWD connector only (CN4 on MB1136, CN11 on MB1360):
// TDI (PA7) and TDO (PA6) are not connected, and T_JRST (PB1) has no
// connector pin. SWO (PA10) is on SWD connector pin 6, but the firmware
// does not use it.
// PWR_ENn (PB15) switches the 5 V of the Nucleo target side: low is on. The
// firmware does not drive it, so a Nucleo that is not broken off gets no
// target power from the ST-Link USB. A broken-off board does not use it.
// No JTAG fast path, no power switch and no key. The bootloader reads SWO
// (PA10) as its key at power-up, so the firmware keeps PA10 for SWO.
// USB_RENUMn (PA15) drives
// an NPN emitter follower to the D+ pull-up: high connects, low
// disconnects. Open-drain: when released, the base divider (10k to 5 V,
// 36k to GND) lifts D+ to about 2.9 V at the host. A 3.3 V push-pull
// output gives about 2.4 V. PA15 is 5 V tolerant.
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
   // the resistor from SWD connector pin 1 (VDD_TARGET) is not fitted. A
   // board that is broken off the Nucleo reads 0 V.
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
   .usbPullUpOpenDrain = true,
};
static_assert(isValid(stLinkV21));

} // namespace board
