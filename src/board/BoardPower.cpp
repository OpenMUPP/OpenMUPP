// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardPower.h"

#if defined(__riscv)
#include <peripherals/Adc.h>
#else
#include <Adc.h>
#endif

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/VrefScale.h"

namespace board {

namespace {

using hw::GpioPin;

// A source with the divider of the board has a high impedance: the longest
// sample time keeps the error small.
#if defined(__riscv)
constexpr Adc::SamplingCycles sampling = Adc::SamplingCycles::ADCSampling239_5;
#elif defined(STM32F1)
constexpr Adc::SamplingCycles sampling = Adc::SamplingCycles::Cycles239_5;
#else
constexpr Adc::SamplingCycles sampling = Adc::SamplingCycles::Cycles640_5;
#endif

// The mean of some conversions removes the noise of the supply.
constexpr unsigned conversions = 8;

#if defined(__riscv)
// One software-triggered conversion of a regular channel. false when it does not end.
bool convert(const Adc &adc, Adc::Channel channel, uint16_t &raw) {
   adc.configureRegularChannel(0, channel);
   adc.setRegularConversionCount(1);
   adc.setRegularConversionTrigger(Adc::RegularTrigger::SWSTART);
   adc.start();
   // 239.5 + 12.5 cycles at 9 MHz are 28 us. The wait is far longer, and ends in a few ms at 144 MHz.
   for(uint32_t wait = 0; wait < 100000; ++wait) {
      if(adc.isConversionComplete()) {
         raw = adc.value();
         return true;
      }
   }
   return false;
}
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
#if defined(__riscv)
   Adc adc(ADC1); // calibrate() is not const
   adc.enableClock();
   // PCLK2 is 72 MHz (initialize_hardware.cpp): /8 is 9 MHz, under the 14 MHz limit of the ADC
   Adc::setClockDivider(Adc::ClockDivider::div8);
   adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
   adc.enable();
   adc.calibrate();
   adcReady_ = true;
#else
   Adc::enableClock();
   Adc::configureClock();
   const Adc adc(ADC1);
#if defined(STM32F1)
   // The STM32F103 calibrates an enabled ADC (RM0008 11.4)
   adc.enable();
   adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
   adcReady_ = adc.calibrate();
#else
   // The STM32H5 calibrates a disabled ADC (RM0481 28.4.8)
   adcReady_ = adc.powerUp() && adc.calibrate() && adc.enable();
   adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
#endif
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
   const Adc adc(ADC1);
   const auto channel = static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin));
   uint32_t sum = 0;
   for(unsigned i = 0; i < conversions; ++i) {
      uint16_t raw = 0;
#if defined(__riscv)
      if(!convert(adc, channel, raw)) {
#else
      if(!adc.convert(channel, raw)) {
#endif
         return std::nullopt;
      }
      sum += raw;
   }
   return vrefMillivolts((sum + conversions / 2) / conversions, current.vref->divider);
}

} // namespace board
