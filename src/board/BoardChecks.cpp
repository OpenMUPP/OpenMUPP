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

// Only the CMSIS-DAP processor keeps the pins of a board without debug
// outputs (main.cpp). The Versaloon and WCH-Link personalities would drive
// them.
#if defined(MUPP_WITH_VERSALOON) || defined(MUPP_WITH_WCH_LINK)
static_assert(board::current.debugOutputs, "this board allows only the cmsis-dap personality: its debug pins must not become outputs");
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
