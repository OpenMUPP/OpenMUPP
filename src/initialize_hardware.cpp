// SPDX-License-Identifier: GPL-3.0-or-later
// Board start-up: clocks, the debug port (SWJ) of the dongle MCU and the
// board pins. The start-up code of system-stm32 or ch32v00x-system calls
// os_startup_initialize_hardware() after the DATA and BSS init, before the
// static constructors and main(). On the STM32H5 it first starts the ROM
// bootloader on request (boot/Bootloader.h).
#include <startup-hooks.h>

#include <cmsis_device.h>
#include <delaybusywait.h>
#include <peripherals/GpioPin.h>
#if !defined(CH32V30x_D8C)
#include <peripherals/Rcc.h>
#endif

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/Leds.h"
#include "boot/Bootloader.h"

using hw::GpioPin;

namespace {

bool clockFromCrystal = false;

void configureClocks() {
#if defined(STM32F1)
   // SYSCLK 72 MHz, USB 48 MHz. On failure the clock stays on HSI.
   clockFromCrystal = Rcc::setClock72MhzHSE(board::current.hseFrequency);
   SystemCoreClockUpdate();
#elif defined(CH32V30x_D8C)
   // SystemInit() of ch32v00x-system (os_startup_initialize_hardware_early())
   // set SYSCLK to 144 MHz from the crystal through the PLL. On failure the
   // clock stayed on HSI, so the PLL as SYSCLK source means the crystal runs.
   SystemCoreClockUpdate();
   clockFromCrystal = (RCC->CFGR0 & RCC_CFGR0_SWS_MASK) == RCC_CFGR0_SWS_PLL;
#else
   // SYSCLK 250 MHz from the crystal (HSE_VALUE, board/BoardChecks.cpp).
   // On failure the clock stays on HSI. USB runs from the HSI48 and the
   // CRS (usb/UsbStack.cpp).
   clockFromCrystal = Rcc::setClock250MhzHSE();
   // The ICACHE must skip the read-only flash (UID for the USB serial
   // number), else the read faults.
   Rcc::disableCacheOfReadOnlyFlash();
   Rcc::enableICache();
#endif
   SysTick_Config(SystemCoreClock / 1000); // 1 ms tick

   // The STM32F1 boards have a 48-pin package: ports A to C. Port D has
   // only the crystal pins. The WeAct STM32H523 and the CH32V307 pin map
   // (plan D10) use ports A to C too.
#if defined(CH32V30x_D8C)
   RCC->APB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN;
#else
   Rcc::enableGPIOAClock();
   Rcc::enableGPIOBClock();
   Rcc::enableGPIOCClock();
#endif
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
      board::configureOutput(*def.usbPullUp, false,
                             def.usbPullUpOpenDrain ? GpioPin::OutputType::OpenDrain : GpioPin::OutputType::PushPull);
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

#if defined(STM32H5)
// A key that is down at the reset starts the ROM bootloader, as
// openmupp-bootloader does on the STM32F1 boards. The request and the
// reset of enterBootloader() put the clocks back into their reset state
// for the jump.
void enterBootloaderOnKey() {
   if(const auto key = board::current.key) {
      delayuS(10); // the pull of the key pin settles
      if(board::isAsserted(*key)) {
         boot::enterBootloader();
      }
   }
}
#endif

} // namespace

bool hardware::isClockFromCrystal() {
   return clockFromCrystal;
}

void os_startup_initialize_hardware() {
#if defined(STM32H5)
   boot::startRomBootloaderOnRequest();
#endif
   configureClocks();

   // Set the debug port before the pins: the JTAG-DP and the SW-DP own
   // their pins (PA13-PA15, PB3, PB4) until SWJ_CFG releases them. The
   // STM32H5 has no SWJ_CFG: the GPIO mode of a pin releases it.
#if defined(STM32F1)
   hw::Afio::setSwjConfig(board::afioSwjConfig(board::swjConfig(board::current)));
#endif

   configurePins();
#if defined(STM32H5)
   enterBootloaderOnKey();
#endif

   // openmupp-bootloader jumps to the firmware with PRIMASK set (cpsid i).
   // Nothing else clears it, so without this the SysTick and USB interrupts
   // never run behind the bootloader.
   __enable_irq();
}
