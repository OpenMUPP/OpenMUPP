// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardPower.h"

#include <Adc.h>
#if defined(CH32V30x_D8C)
#include <cmsis_device.h>
#include <utils/Interrupt.h>
#endif

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/VrefScale.h"

namespace board {

namespace {

using hw::GpioPin;

// A source with the divider of the board has a high impedance: the longest
// sample time keeps the error small.
#if defined(STM32F1) || defined(__riscv)
constexpr Adc::SamplingCycles sampling = Adc::SamplingCycles::Cycles239_5;
#else
constexpr Adc::SamplingCycles sampling = Adc::SamplingCycles::Cycles640_5;
#endif

// The mean of some conversions removes the noise of the supply.
constexpr unsigned conversions = 8;

#if defined(CH32V30x_D8C)
// PCLK2 is HCLK (144 MHz, initialize_hardware.cpp) for fast GPIO. ADCPRE divides by 8 at most, so the ADC
// clock would be 18 MHz, above the 14 MHz limit (datasheet fADC). Thus PCLK2 is HCLK / 2 (ADC 12 MHz) only
// while the ADC runs, and the ADC clock is off at other times. USART1 (UART bridge) is also on APB2: its
// divider changes with PCLK2, so its baud rate stays the same (an odd divider rounds up). At each switch,
// one bit on the line is up to about 110 ns longer or shorter (measured, docs/boards.md).
class AdcClockWindow {
   public:
   AdcClockWindow() {
      Interrupt::CriticalSection cs;
      brr_ = USART1->BRR;
      RCC->CFGR0 = (RCC->CFGR0 & ~RCC_CFGR0_PPRE2_MASK) | (4ul << 11);
      if(brr_ != 0) {
         USART1->BRR = (brr_ + 1) / 2;
      }
      Adc::enableClock();
   }

   ~AdcClockWindow() {
      Interrupt::CriticalSection cs;
      Adc::disableClock();
      RCC->CFGR0 &= ~RCC_CFGR0_PPRE2_MASK;
      if(brr_ != 0) {
         USART1->BRR = brr_;
      }
   }

   AdcClockWindow(const AdcClockWindow &) = delete;
   AdcClockWindow &operator=(const AdcClockWindow &) = delete;

   private:
   uint32_t brr_ = 0;
};
#else
// The ADC clock is within its limit at the bus clock of the board.
struct AdcClockWindow {};
#endif

} // namespace

void BoardPower::init() {
   if(current.powerSwitch) {
      // The supply is off: the switch pin floats.
      gpioPin(current.powerSwitch->pin).configure(GpioPin::Mode::Input);
   }
   if(!current.vref) {
      return;
   }
   gpioPin(current.vref->pin).configure(GpioPin::Mode::Analog);
   const AdcClockWindow window;
   Adc::enableClock();
   Adc::configureClock();
   const Adc adc(ADC1);
#if defined(STM32F1) || defined(__riscv)
   // The STM32F103 and the CH32V307 calibrate an enabled ADC (RM0008 11.4)
   adc.enable();
   adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
   adcReady_ = adc.calibrate();
#else
   // The STM32H5 calibrates a disabled ADC (RM0481 28.4.8)
   adcReady_ = adc.powerUp() && adc.calibrate() && adc.enable();
   adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
#endif
}

bool BoardPower::hasSwitch() const {
   return current.powerSwitch.has_value();
}

void BoardPower::setSwitch(bool on) {
   if(!current.powerSwitch) {
      return;
   }
   if(on) {
      // The output register first, so the pin starts at the active level.
      configureOutput(Signal{ current.powerSwitch->pin, current.powerSwitch->on }, true);
   } else {
      gpioPin(current.powerSwitch->pin).configure(GpioPin::Mode::Input);
   }
}

bool BoardPower::hasSense() const {
   return current.vref.has_value();
}

std::optional<uint16_t> BoardPower::senseMillivolts() {
   if(!current.vref || !adcReady_) {
      return std::nullopt;
   }
   const AdcClockWindow window;
   const Adc adc(ADC1);
   const auto channel = static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin));
   uint32_t sum = 0;
   for(unsigned i = 0; i < conversions; ++i) {
      uint16_t raw = 0;
      if(!adc.convert(channel, raw)) {
         return std::nullopt;
      }
      sum += raw;
   }
   return vrefMillivolts((sum + conversions / 2) / conversions, current.vref->divider);
}

} // namespace board
