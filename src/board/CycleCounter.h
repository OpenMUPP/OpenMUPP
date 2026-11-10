// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The core-clock cycle counter, the time base of the bit-bang engines
// (jtag/JtagBitBang.h, swd/SwdBitBang.h). Target only. On the STM32 it is the
// DWT counter. The QingKe V4F of the CH32V307 has no cycle CSR, so it is the
// low word of the SysTick counter, which ch32v00x-system runs free at the
// core clock (utils/Timekeeping.h).

#include <cmsis_device.h>
#include <cstdint>

namespace board {

// Starts the cycle counter if it does not run. A debugger can stop it, and
// a stopped counter would make the timed waits endless. Call it before
// each bit-bang operation.
[[gnu::always_inline]] inline void startCycleCounter() {
#if defined(__riscv)
   // SysTick_Config() started the counter, and nothing stops it.
#elif defined(DCB)
   // Armv8-M (STM32H5): the CMSIS core header has DCB, not CoreDebug.
   DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
   CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
#if !defined(__riscv)
   DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
#endif
}

// Core clock cycles, free-running
[[gnu::always_inline]] inline uint32_t cycleCount() {
#if defined(__riscv)
   return SysTick->CNT;
#else
   return DWT->CYCCNT;
#endif
}

} // namespace board
