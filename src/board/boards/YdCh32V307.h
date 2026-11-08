// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board/BoardDefinition.h"

namespace board {

// YD-CH32V307VCT6 development board: CH32V307VCT6 (LQFP100), 8 MHz
// crystal, USB high speed on PB6 and PB7. The pin map (plan D10) uses only
// pins that the LQFP64 and QFN68 packages also have, for a future probe:
// - TCK, TDI and TDO are on the SPI2 pins (PB13, PB15, PB14), as on the
//   STM32F1 boards. One pin, PC11, is SWDIO out and in.
// - JTAG fast path: TMS is on SPI3 MISO (PC11, remap). It needs a link
//   from TCK (PB13) to SPI3 SCK (PC10): a 330-470 Ohm resistor, which
//   limits the current if other code drives PC10. Keep the microSD slot
//   empty: PC10 and PC11 are also its DAT2 and DAT3.
// - The on-board LEDs are active high: LED2 (green, PB4) shows the state,
//   LED1 (red, PA15) the USB state. The USR key (PB3) is active low.
//
// 3.3 V targets only: no level shifters, no power switch. The USB pull-up
// is in the USBHS controller.
inline constexpr Definition ydCh32V307{
   .name = "ydch32v307",
   .hseFrequency = 8'000'000,
   .family = Family::Ch32V30x,
   // The ID of the Mini: the host tools know it
   .hardwareId = 0x15,
   .debug{
   .tck{ Port::B, 13 },
   .tmsOut{ Port::C, 11 },
   .tmsIn{ Port::C, 11 },
   .tdi{ Port::B, 15 },
   .tdo{ Port::B, 14 },
   .trst = Pin{ Port::B, 9 },
   .srst{ Port::B, 12 },
   .rtck = Pin{ Port::A, 8 },
   },
   .jtagFastPath = JtagFastPath{ .tckLink{ Port::C, 10 } },
   .uart = Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } },
   // Optional: wire the target supply (3.3 V maximum) to PB0
   .vref = VrefSense{ .pin{ Port::B, 0 }, .divider = 1 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   .state = Signal{ { Port::B, 4 }, Active::High },
   .usb = Signal{ { Port::A, 15 }, Active::High },
   },
   .key = Signal{ { Port::B, 3 }, Active::Low },
   .usbPullUp = std::nullopt,
};

static_assert(isValid(ydCh32V307));

} // namespace board
