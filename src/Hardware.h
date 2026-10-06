#pragma once

// State of the hardware after os_startup_initialize_hardware()
// (src/initialize_hardware.cpp).

namespace hardware {

// True when the system clock runs at 72 MHz from the crystal. False when the
// crystal or the PLL did not start: the clock then stays on HSI (8 MHz).
bool isClockFromCrystal();

} // namespace hardware
