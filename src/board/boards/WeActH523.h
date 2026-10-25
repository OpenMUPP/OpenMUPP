#pragma once

#include "board/BoardDefinition.h"

namespace board {

// WeAct Studio STM32H523 core board (V1.0, "Black Pill" form): STM32H523CCU6
// or STM32H523CCT6, 8 MHz crystal. The pin map of the NUCLEO-H533RE (#89),
// with these changes for the board and its headers:
// - The on-board LED (PC13) and key (PA0) are active low.
// - RTCK moves from PA0 (the key) to PA8.
// - SWDIO in is PB12 and SRST is PB5, so header P2 has SWDIO, SWCLK, TDO,
//   TDI, RTCK, TX, RX on pins 1 to 7. docs/boards.md has the wiring.
//
// 3.3 V targets only: no level shifters, no power switch. The USB D+
// pull-up is internal. SWDIO uses two pins with a wire between them (PA6 to
// PB12), as on the Mini. The JTAG fast path needs the TCK link (PB13 to PB3)
// and the H5 SPI driver (#97): not yet.
inline constexpr Definition weActH523{
   .name = "weacth523",
   .hseFrequency = 8'000'000,
   .family = Family::Stm32H5,
   // The ID of the Mini: the host tools know it
   .hardwareId = 0x15,
   .debug{
   .tck{ Port::B, 13 },
   .tmsOut{ Port::A, 6 },
   .tmsIn{ Port::B, 12 },
   .tdi{ Port::B, 15 },
   .tdo{ Port::B, 14 },
   .trst = Pin{ Port::B, 4 },
   .srst{ Port::B, 5 },
   .rtck = Pin{ Port::A, 8 },
   },
   .jtagFastPath = std::nullopt,
   .uart = Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } },
   // Optional: wire the target supply (3.3 V maximum) to PB0
   .vref = VrefSense{ .pin{ Port::B, 0 }, .divider = 1 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   .state = Signal{ { Port::C, 13 }, Active::Low },
   .usb = std::nullopt,
   },
   .key = Signal{ { Port::A, 0 }, Active::Low },
   .usbPullUp = std::nullopt,
};

static_assert(isValid(weActH523));

} // namespace board
