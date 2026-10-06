// Build-time checks of the selected board against the build configuration.

#include "board/Board.h"
#include "board/BoardPins.h"

// CMake sets HSE_VALUE (SYSTEM_STM32_HSE_VALUE) from BOARD. The CMSIS clock
// code and the board definition must agree on the crystal.
static_assert(HSE_VALUE == board::current.hseFrequency, "HSE_VALUE does not match the board crystal");

// The debug pins map to FastPin. The JTAG and SWD engines use them so.
static_assert(board::FastPinOf<board::current.debug.tck>::mask != 0);
static_assert(board::FastPinOf<board::current.debug.tmsOut>::mask != 0);
static_assert(board::FastPinOf<board::current.debug.tmsIn>::mask != 0);
