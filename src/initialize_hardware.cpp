// Board start-up: clocks, the debug port (SWJ) of the dongle MCU and the
// board pins. The system-stm32 start-up code calls
// os_startup_initialize_hardware() after the DATA and BSS init, before the
// static constructors and main().
#include <startup-hooks.h>

#include <cmsis_device.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/Leds.h"

using hw::GpioPin;

namespace {

bool clockFromCrystal = false;

void configureClocks() {
   // SYSCLK 72 MHz, USB 48 MHz. On failure the clock stays on HSI.
   clockFromCrystal = Rcc::setClock72MhzHSE(board::current.hseFrequency);
   SystemCoreClockUpdate();
   SysTick_Config(SystemCoreClock / 1000); // 1 ms tick

   // All boards have an STM32F103 in a 48-pin package: ports A to C.
   // Port D has only the crystal pins.
   Rcc::enableGPIOAClock();
   Rcc::enableGPIOBClock();
   Rcc::enableGPIOCClock();
}

// Puts the board pins into a safe state. The JTAG, SWD, UART and Vref pins
// stay inputs (reset state) until their drivers configure them.
void configurePins() {
   const auto &def = board::current;

   board::initializeLeds();
   board::setLed(board::Led::Power, true);

   // USB D+ pull-up off: the host does not see the device before the USB
   // stack starts.
   if(def.usbPullUp) {
      board::configureOutput(*def.usbPullUp, false);
   }

   // Target power off: the switch pin floats.
   if(def.powerSwitch) {
      board::gpioPin(def.powerSwitch->pin).configure(GpioPin::Mode::Input);
   }

   // Pull the key to its released level.
   if(def.key) {
      const auto pull = def.key->levelFor(false) ? GpioPin::Pull::Up : GpioPin::Pull::Down;
      board::gpioPin(def.key->pin).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, pull);
   }
}

} // namespace

bool hardware::isClockFromCrystal() {
   return clockFromCrystal;
}

void os_startup_initialize_hardware() {
   configureClocks();

   // Set the debug port before the pins: the JTAG-DP and the SW-DP own
   // their pins (PA13-PA15, PB3, PB4) until SWJ_CFG releases them. The
   // STM32H5 has no SWJ_CFG: the GPIO mode of a pin releases it.
#if defined(STM32F1)
   hw::Afio::setSwjConfig(board::afioSwjConfig(board::swjConfig(board::current)));
#endif

   configurePins();

   // versaloon-bootloader jumps to the firmware with PRIMASK set (cpsid i).
   // Nothing else clears it, so without this the SysTick and USB interrupts
   // never run behind the bootloader.
   __enable_irq();
}
