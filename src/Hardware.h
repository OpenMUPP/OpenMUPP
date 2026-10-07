#pragma once

// State of the hardware after os_startup_initialize_hardware()
// (src/initialize_hardware.cpp).

#include <cstdint>

namespace hardware {

// True when the system clock runs at 72 MHz from the crystal. False when the
// crystal or the PLL did not start: the clock then stays on HSI (8 MHz).
bool isClockFromCrystal();

// NVIC priorities (lower value = more urgent). The USART receive register
// holds one byte: at 2 Mbaud the interrupt must read it within 5 us, so the
// UART interrupt preempts the USB interrupt. The SysTick keeps the lowest
// priority that SysTick_Config() gives it.
inline constexpr uint32_t uartIrqPriority = 0;
inline constexpr uint32_t usbIrqPriority = 1;

} // namespace hardware
