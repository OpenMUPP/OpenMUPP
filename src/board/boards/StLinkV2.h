#pragma once

#include "board/BoardDefinition.h"

namespace board {

// ST-Link/V2 (STM32F103C8, 8 MHz crystal). Compile-only target: it keeps
// the board abstraction honest, but nobody has tested it on hardware.
// Check the pins on the real board before a port.
//
// No JTAG fast path (TCK is not on SPI2 SCK and there is no PB3 link), no
// power switch, no key, and a fixed USB pull-up. TCK and SWCLK are PA5
// (PB13 is connected to it on some boards). The UART is USART2 on PA2/PA3.
inline constexpr Definition stLinkV2{
   .name = "stlinkv2",
   .hseFrequency = 8'000'000,
   // ST_Link in the original Versaloon firmware
   .hardwareId = 0x38,
   .debug{
   .tck{ Port::A, 5 },
   .tmsOut{ Port::B, 14 },
   .tmsIn{ Port::B, 14 },
   .tdi{ Port::A, 7 },
   .tdo{ Port::A, 6 },
   .trst = std::nullopt,
   .srst{ Port::B, 0 },
   .rtck = std::nullopt,
   },
   .jtagFastPath = std::nullopt,
   .uart = Uart{ .usart = 2, .tx{ Port::A, 2 }, .rx{ Port::A, 3 } },
   .vref = VrefSense{ .pin{ Port::A, 0 }, .adcChannel = 0, .divider = 2 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   .state = Signal{ { Port::A, 9 }, Active::High },
   .usb = std::nullopt,
   },
   .key = std::nullopt,
   .usbPullUp = std::nullopt,
};
static_assert(isValid(stLinkV2));

} // namespace board
