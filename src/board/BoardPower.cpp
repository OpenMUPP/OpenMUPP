// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardPower.h"

#include <Adc.h>
#include <utils/Interrupt.h>
#if defined(CH32V30x_D8C)
#include <cmsis_device.h>
#include <peripherals/Rcc.h>
#include <utils/GpioInterrupt.h>
#endif

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/VrefScale.h"

namespace board {

namespace {

using hw::GpioPin;

// Set by the fault interrupt, cleared by takeFault()
volatile bool faulted = false;

// Both target switches off. In the fault interrupt too.
void targetSupplyOff() {
   const auto &supply = *current.targetSupply;
   gpioPin(supply.enable3v3).write(false);
   gpioPin(supply.enable5v).write(false);
}

#if defined(CH32V30x_D8C)
// The fault interrupt of PA11 and PA12. The TPS2553 does not latch off, so
// the interrupt switches both off at once. The main loop then takes the
// fault (takeFault()).
void faultInterrupt(void * /*arg*/) {
   targetSupplyOff();
   faulted = true;
}
#endif

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
// while the ADC runs, and the ADC clock is off at other times. The UART bridge (UART7) is on APB1, so its
// baud rate does not change.
class AdcClockWindow {
   public:
   AdcClockWindow() {
      Interrupt::CriticalSection cs;
      Rcc::setAPB2Prescaler(Rcc::APBPrescaler::div2);
      Adc::enableClock();
   }

   ~AdcClockWindow() {
      Interrupt::CriticalSection cs;
      Adc::disableClock();
      Rcc::setAPB2Prescaler(Rcc::APBPrescaler::div1);
   }

   AdcClockWindow(const AdcClockWindow &) = delete;
   AdcClockWindow &operator=(const AdcClockWindow &) = delete;
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
   if(current.targetSupply) {
      // The start-up switched both off and turned on the pull-ups of the
      // fault inputs (initialize_hardware.cpp).
      targetSupplyOff();
      gpioPin(current.targetSupply->v5Sense.pin).configure(GpioPin::Mode::Analog);
#if defined(CH32V30x_D8C)
      for(const auto fault : { current.targetSupply->fault3v3, current.targetSupply->fault5v }) {
         const auto port = reinterpret_cast<GPIO_TypeDef *>(portBase(fault.port));
         // A pin of the same number on another port can take the line. No
         // board does this: then the fault stays a level on the input only.
         (void)GpioInterrupt::connectHandler(&faultInterrupt, nullptr, port, fault.number, Exti::TriggerEdge::Falling);
      }
#endif
   }
   if(current.idSense) {
      // Analog: no internal pull, which would move the ratio
      gpioPin(current.idSense->adapterId).configure(GpioPin::Mode::Analog);
      gpioPin(current.idSense->boardId).configure(GpioPin::Mode::Analog);
   }
   if(current.vpp) {
      // The VPP senses (board/BoardVpp.h) and the DAC pin, high-impedance
      for(const Pin pin : { current.vpp->sense, current.vpp->sense2, current.vpp->dac }) {
         gpioPin(pin).configure(GpioPin::Mode::Analog);
      }
   }
   if(!current.vref) {
      return;
   }
   gpioPin(current.vref->pin).configure(GpioPin::Mode::Analog);
   const AdcClockWindow window;
   Adc::enableClock();
   Adc::configureClock();
   const Adc adc(ADC1);
   const auto setSampling = [&adc] {
      adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.vref->pin)), sampling);
      if(current.targetSupply) {
         adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, current.targetSupply->v5Sense.pin)), sampling);
      }
      if(current.idSense) {
         for(const Pin pin : { current.idSense->adapterId, current.idSense->boardId }) {
            adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, pin)), sampling);
         }
      }
      if(current.vpp) {
         for(const Pin pin : { current.vpp->sense, current.vpp->sense2 }) {
            adc.setSamplingTime(static_cast<Adc::Channel>(*adcChannelOf(current.family, pin)), sampling);
         }
      }
   };
#if defined(STM32F1) || defined(__riscv)
   // The STM32F103 and the CH32V307 calibrate an enabled ADC (RM0008 11.4)
   adc.enable();
   setSampling();
   adcReady_ = adc.calibrate();
#else
   // The STM32H5 calibrates a disabled ADC (RM0481 28.4.8)
   adcReady_ = adc.powerUp() && adc.calibrate() && adc.enable();
   setSampling();
#endif
}

bool BoardPower::hasSwitch() const {
   return current.powerSwitch.has_value() || current.targetSupply.has_value();
}

bool BoardPower::has5vSwitch() const {
   return current.targetSupply.has_value();
}

void BoardPower::select(protocol::Supply supply) {
   if(current.targetSupply) {
      // Not between the two writes: a fault interrupt there would switch
      // off, and the second write could switch on again.
      const Interrupt::CriticalSection cs;
      targetSupplyOff();
      if(supply == protocol::Supply::V3v3) {
         gpioPin(current.targetSupply->enable3v3).write(true);
      } else if(supply == protocol::Supply::V5) {
         gpioPin(current.targetSupply->enable5v).write(true);
      }
      return;
   }
   if(!current.powerSwitch) {
      return;
   }
   if(supply == protocol::Supply::V3v3) {
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
   if(!current.vref) {
      return std::nullopt;
   }
   return measure(*current.vref);
}

std::optional<uint16_t> BoardPower::measure(const VrefSense &sense) const {
   const auto raw = meanRaw(sense.pin);
   if(!raw) {
      return std::nullopt;
   }
   return vrefMillivolts(*raw, sense.divider);
}

std::optional<uint16_t> BoardPower::meanRaw(Pin pin) const {
   const auto adcChannel = adcChannelOf(current.family, pin);
   if(!adcReady_ || !adcChannel) {
      return std::nullopt;
   }
   const AdcClockWindow window;
   const Adc adc(ADC1);
   const auto channel = static_cast<Adc::Channel>(*adcChannel);
   uint32_t sum = 0;
   for(unsigned i = 0; i < conversions; ++i) {
      uint16_t raw = 0;
      if(!adc.convert(channel, raw)) {
         return std::nullopt;
      }
      sum += raw;
   }
   return static_cast<uint16_t>((sum + conversions / 2) / conversions);
}

std::optional<uint16_t> BoardPower::fiveVoltInputMillivolts() {
   if(!current.targetSupply) {
      return std::nullopt;
   }
   return measure(current.targetSupply->v5Sense);
}

bool BoardPower::takeFault() {
   const Interrupt::CriticalSection cs;
   const bool taken = faulted;
   faulted = false;
   return taken;
}

} // namespace board
