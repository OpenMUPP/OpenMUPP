// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The core-clock cycle counter, the time base of the bit-bang engines
// (jtag/JtagBitBang.h, swd/SwdBitBang.h). Target only. On the STM32 it is the
// DWT counter. The QingKe V4F of the CH32V307 has no counting mcycle CSR (it
// reads 0 on hardware), so it is hw::CycleCounter of ch32v00x-system: the low
// word of the SysTick counter, which runs free at the core clock.

#include <cmsis_device.h>
#include <cstdint>
#if defined(__riscv)
#include <peripherals/CycleCounter.h>
#endif

namespace board {

// Starts the cycle counter if it does not run. A debugger can stop it, and
// a stopped counter would make the timed waits endless. Call it before
// each bit-bang operation.
[[gnu::always_inline]] inline void startCycleCounter() {
#if defined(__riscv)
   hw::CycleCounter::start();
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
   return hw::CycleCounter::count();
#else
   return DWT->CYCCNT;
#endif
}

} // namespace board
