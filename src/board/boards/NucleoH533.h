#pragma once

#include "board/BoardDefinition.h"

namespace board {

// NUCLEO-H533RE (MB1814, UM3121): STM32H533RET6, 24 MHz crystal. The pin
// map is the pin map of the STM32H523 probe board (#89), so the two
// definitions stay almost the same. The target pins are on the Arduino and
// Morpho headers; docs/boards.md has the wiring.
//
// 3.3 V targets only: no level shifters, no power switch. The only user LED
// is LD2 (PA5), the state LED. The USB D+ pull-up is internal. SWDIO uses
// two pins with a wire between them, as on the Mini. The JTAG fast path
// needs the TCK link (PB13 to PB3) and the H5 SPI driver (#97): not yet.
inline constexpr Definition nucleoH533{
   .name = "nucleoh533",
   .hseFrequency = 24'000'000,
   .family = Family::Stm32H5,
   // The ID of the Mini: the host tools know it
   .hardwareId = 0x15,
   .debug{
   .tck{ Port::B, 13 },
   .tmsOut{ Port::A, 6 },
   .tmsIn{ Port::A, 7 },
   .tdi{ Port::B, 15 },
   .tdo{ Port::B, 14 },
   .trst = Pin{ Port::B, 4 },
   .srst{ Port::B, 10 },
   .rtck = Pin{ Port::A, 0 },
   },
   .jtagFastPath = std::nullopt,
   .uart = Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } },
   // Optional: wire the target supply (3.3 V maximum) to PB0
   .vref = VrefSense{ .pin{ Port::B, 0 }, .divider = 1 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   .state = Signal{ { Port::A, 5 }, Active::High },
   .usb = std::nullopt,
   },
   .key = Signal{ { Port::C, 13 }, Active::High },
   .usbPullUp = std::nullopt,
};

static_assert(isValid(nucleoH533));

} // namespace board
