// SPDX-License-Identifier: GPL-3.0-or-later
// Build-time checks of the selected board against the build configuration.

#include "board/Board.h"
#include "board/BoardPins.h"

// CMake sets STM32_FAMILY (the define STM32F1, STM32H5, ...) or the CH32
// device macro (CH32_DEVICE) from BOARD.
#if defined(STM32F1)
static_assert(board::current.family == board::Family::Stm32F1, "the board is not an STM32F1 board");
#elif defined(STM32H5)
static_assert(board::current.family == board::Family::Stm32H5, "the board is not an STM32H5 board");
#elif defined(CH32V30x_D8C)
static_assert(board::current.family == board::Family::Ch32V30x, "the board is not a CH32V307 board");
#else
#error "Unsupported MCU family"
#endif

// CMake sets HSE_VALUE (SYSTEM_STM32_HSE_VALUE or CH32V00X_SYSTEM_HSE_VALUE)
// from BOARD. The CMSIS clock
// code and the board definition must agree on the crystal. A board without
// a crystal does not use HSE_VALUE.
static_assert(board::current.hseFrequency == 0 || HSE_VALUE == board::current.hseFrequency, "HSE_VALUE does not match the board crystal");

// The debug pins map to FastPin. The JTAG and SWD engines use them so.
static_assert(board::FastPinOf<board::current.debug.tck>::mask != 0);
static_assert(board::FastPinOf<board::current.debug.tmsOut>::mask != 0);
static_assert(board::FastPinOf<board::current.debug.tmsIn>::mask != 0);

// The SWO pin is the default RX pin of its USART: USART1 RX on PA10, USART2
// RX on PA3, USART3 RX on PB11. The STM32F103 (RM0008 9.3.8), the STM32H5
// (DS14539 table 16, AF7) and the CH32V307 (CH32FV2x_V3xRM table 10-16) have
// the same pins. The receiver does not set a USART remap.
namespace {
constexpr bool isSwoRxPin(const board::Swo &swo) {
   using board::Pin;
   using board::Port;
   switch(swo.usart) {
   case 1:
      return swo.rx == Pin{ Port::A, 10 };
   case 2:
      return swo.rx == Pin{ Port::A, 3 };
   case 3:
      return swo.rx == Pin{ Port::B, 11 };
   default:
      return false;
   }
}
} // namespace
static_assert(!board::current.swo || isSwoRxPin(*board::current.swo), "the SWO pin is not the RX pin of its USART");
