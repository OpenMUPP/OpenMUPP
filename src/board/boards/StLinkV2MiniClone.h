#pragma once

#include "board/BoardDefinition.h"

namespace board {

// ST-Link/V2 mini clone: the USB stick dongle with a 2x5 header, sold as
// "ST-Link V2 mini" (STM32F103, 8 MHz crystal, no markings). It is not the
// official ST-Link/V2 design (StLinkV2.h). Measured pin map:
// docs/stlinkv2-mini-clone.md.
//
// The board ties pins together: PA5 + PB13 (SWCLK), PB14 + PB12
// (SWDIO), PB6 + PB5 (RST) and PB7-PB11 (SWIM). The firmware drives one pin
// of each group; the other pins stay a floating input after the reset.
// TDI, TDO and the UART are not on the header: these pins are not
// connected, so JTAG and the UART bridge do not reach a target.
// No power switch, no key, and a fixed USB pull-up.
inline constexpr Definition stLinkV2MiniClone{
   .name = "stlinkv2miniclone",
   .hseFrequency = 8'000'000,
   .family = Family::Stm32F1,
   // ST_Link in the original Versaloon firmware, as the official design
   .hardwareId = 0x38,
   .debug{
   .tck{ Port::A, 5 },
   .tmsOut{ Port::B, 14 },
   .tmsIn{ Port::B, 14 },
   .tdi{ Port::A, 7 },
   .tdo{ Port::A, 6 },
   .trst = std::nullopt,
   .srst{ Port::B, 6 },
   .rtck = std::nullopt,
   },
   .jtagFastPath = std::nullopt,
   .uart = Uart{ .usart = 2, .tx{ Port::A, 2 }, .rx{ Port::A, 3 } },
   // Divider 2 on PA0. It reads the 3.3 V of the dongle: the header 3.3 V is
   // an output, not a target voltage.
   .vref = VrefSense{ .pin{ Port::A, 0 }, .divider = 2 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   // Two LEDs on PA9: high lights the blue LED, low the red LED.
   .state = Signal{ { Port::A, 9 }, Active::High },
   .usb = std::nullopt,
   },
   .key = std::nullopt,
   .usbPullUp = std::nullopt,
};
static_assert(isValid(stLinkV2MiniClone));

} // namespace board
