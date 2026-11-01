// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The DWT cycle counter, the time base of the bit-bang engines
// (jtag/JtagBitBang.h, swd/SwdBitBang.h). Target only.

#include <cmsis_device.h>
#include <cstdint>

namespace board {

// Starts the cycle counter if it does not run. A debugger can stop it, and
// a stopped counter would make the timed waits endless. Call it before
// each bit-bang operation.
[[gnu::always_inline]] inline void startCycleCounter() {
#if defined(DCB)
   // Armv8-M (STM32H5): the CMSIS core header has DCB, not CoreDebug.
   DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
   CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
   DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// Core clock cycles, free-running
[[gnu::always_inline]] inline uint32_t cycleCount() {
   return DWT->CYCCNT;
}

} // namespace board
