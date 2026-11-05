// SPDX-License-Identifier: GPL-3.0-or-later

// Stub image of the CH32V307 boards (cmake/Ch32StubImage.cmake) until the
// port is complete. The start-up code of ch32v00x-system calls SystemInit()
// (144 MHz from the 8 MHz crystal) before main(). main() blinks LED2 (PB4)
// of the YD-CH32V307VCT6, so the image also shows on hardware that it runs.

#include <peripherals/GpioPin.h>
#include <wch_device.h>

int main() {
   RCC->APB2PCENR |= RCC_IOPBEN;
   const hw::GpioPin led(GPIOB, 4);
   led.configure(hw::GpioPin::Mode::Output, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::Medium);
   while(true) {
      led.toggle();
      // About 0.2 s at 144 MHz; the exact time does not matter
      for(uint32_t i = 0; i < 4'000'000; i = i + 1) {
         asm volatile("");
      }
   }
}
