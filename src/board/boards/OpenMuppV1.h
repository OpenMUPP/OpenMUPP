// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board/BoardDefinition.h"

namespace board {

// OpenMUPP v1 probe board: CH32V307RCT6 (LQFP64), 8 MHz crystal, USB high
// speed on PB6 and PB7, level shifters to the target connector J4. The pin
// map is in docs/hardware.md of the openmupp-v1 repository
// (https://github.com/OpenMUPP/openmupp-v1).
//
// The definition has only the pins that the board layer knows. The board
// layer does not drive the shifter direction pins (DIR_OUT PB10, DIR_SWDIO
// PC3, UART_TX_DIR PC9) and the power pins (target switches, VPP). The
// pull-downs on DIR_OUT and DIR_SWDIO set the shifters from the target to
// the probe, so the shifters drive PB13, PB15, PB9 and PC11. An MCU output
// on these pins would drive against them: debugOutputs is false, so the
// debug engines do not run. Only the UART bridge reaches the target
// (docs/boards.md).
inline constexpr Definition openMuppV1{
   .name = "openmuppv1",
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
   // The gate of Q1: high pulls nRESET low
   .srst{ Port::B, 12 },
   .rtck = Pin{ Port::A, 8 },
   .srstActive = Active::High,
   .srstSense = Pin{ Port::B, 11 },
   },
   // R10 links TCK (PB13) to SPI3 SCK (PC10)
   .jtagFastPath = JtagFastPath{ .tckLink{ Port::C, 10 } },
   // UART7 remap: its RX and TX DMA channels (DMA2 8, 9) do not collide with the JTAG fast path.
   .uart = Uart{ .usart = 7, .tx{ Port::A, 6 }, .rx{ Port::A, 7 } },
   // R5, R6: VTREF / 2
   .vref = VrefSense{ .pin{ Port::B, 0 }, .divider = 2 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   // D4 (red) and D2 (green). D1 (blue, PC4) is not used yet.
   .state = Signal{ { Port::A, 10 }, Active::High },
   .usb = Signal{ { Port::A, 9 }, Active::High },
   },
   // SW1 on the BOOT0 net
   .key = Signal{ { Port::B, 3 }, Active::High },
   .usbPullUp = std::nullopt,
   .debugOutputs = false,
};

static_assert(isValid(openMuppV1));

} // namespace board
