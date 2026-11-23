// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "board/BoardDefinition.h"

namespace board {

// OpenMUPP v1 probe board: CH32V307RCT6 (LQFP64), 8 MHz crystal, USB high
// speed on PB6 and PB7, level shifters to the target connector J4. The pin
// map is in docs/hardware.md of the openmupp-v1 repository
// (https://github.com/OpenMUPP/openmupp-v1).
//
// The start-up (initialize_hardware.cpp) sets the direction pins DIR_OUT
// (PB10) and DIR_SWDIO (PC3) low, so the shifters work from the target to
// the probe and drive PB13, PB15, PB9, PA2, PA0 and PC11. The engines do
// not set the direction pins yet. An MCU output on these pins would drive
// against the shifters: debugOutputs is false, so the debug engines do not
// run. Only the UART bridge reaches the target (docs/boards.md).
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
   // SWO: the TDO net also goes to PA3 (USART2 RX). PB14 cannot receive it.
   .swo = Swo{ .usart = 2, .rx{ Port::A, 3 } },
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
   .shifters{ {
   .dirOut{ Port::B, 10 },
   .dirSwdio{ Port::C, 3 },
   .uartTxDir{ Port::C, 9 },
   .aux1{ Port::A, 2 },
   .aux2{ Port::A, 0 },
   .tckReadBack{ Port::C, 6 },
   .tdiReadBack{ Port::C, 7 },
   .aux1ReadBack{ Port::C, 8 },
   } },
   // U12 (3.3 V) and U14 (5 V). Q4 gives 3.3 V priority. R45, R46: +5V / 2.
   .targetSupply{ {
   .enable3v3{ Port::B, 8 },
   .enable5v{ Port::B, 5 },
   .fault3v3{ Port::A, 11 },
   .fault5v{ Port::A, 12 },
   .v5Sense{ .pin{ Port::C, 0 }, .divider = 2 },
   } },
   // VPP_12V_ADJ on DAC channel 2: a higher DAC voltage gives a lower VPP.
   // PC14 and PC15 are backup-domain pins (3 mA, 2 MHz): static levels only.
   .vpp{ {
   .dac{ Port::A, 5 },
   .enable{ Port::C, 14 },
   .on{ Port::C, 15 },
   .pullDown{ Port::C, 5 },
   .sense{ Port::A, 4 },
   .sense2{ Port::C, 1 },
   } },
   .idSense = IdSense{ .adapterId{ Port::B, 1 }, .boardId{ Port::C, 2 } },
   .debugOutputs = false,
};

static_assert(isValid(openMuppV1));

} // namespace board
