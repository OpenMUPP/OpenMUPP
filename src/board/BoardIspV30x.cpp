// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardIsp.h"

#include <peripherals/Rcc.h>
#include <peripherals/Spi.h>

// The pins and SPI2 of the AVR ISP engine on the CH32V307
// (board/BoardIsp.h). CH32FV2x_V3xRM chapter 20 (SPI).

template class avr::IspEngine<board::IspPins, board::IspSpi>;

namespace board {

namespace {

using hw::GpioPin;
using hw::Spi;

using Sck = FastPinOf<current.debug.tck>;
using Mosi = FastPinOf<current.debug.tdi>;

constexpr Signal resetSignal{ current.debug.srst, current.debug.srstActive };
// Active low: open-drain, the pull-up of the target releases the line.
// Active high: push-pull into the transistor.
constexpr GpioPin::OutputType resetType =
current.debug.srstActive == Active::Low ? GpioPin::OutputType::OpenDrain : GpioPin::OutputType::PushPull;

constexpr uint8_t gpioOutput =
hw::gpio_f1::pinConfig(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
constexpr uint8_t spiOutput =
hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);

Spi spi() {
   return Spi{ SPI2 };
}

constexpr Spi::BaudRatePrescaler prescalerOf(uint8_t shift) {
   return static_cast<Spi::BaudRatePrescaler>(static_cast<uint32_t>(shift - 1u) * SPI_CTLR1_BR_0);
}

// The output register first, so the pin starts at its level.
void configureOutput(Pin pin, bool level) {
   const GpioPin gpio = gpioPin(pin);
   gpio.write(level);
   gpio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
}

} // namespace

void IspPins::init() {
   const DebugPins &pins = current.debug;
   // RESET first: SCK must be low when RESET goes low, and the target
   // must not run while SCK and MOSI change.
   reset(true);
   configureOutput(pins.tck, false);
   configureOutput(pins.tdi, false);
   gpioPin(pins.tdo).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
}

void IspPins::fini() {
   const DebugPins &pins = current.debug;
   for(const Pin pin : { pins.tck, pins.tdi }) {
      gpioPin(pin).configure(GpioPin::Mode::Input);
   }
   // The target runs.
   reset(false);
   if(current.debug.srstActive == Active::Low) {
      gpioPin(pins.srst).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
}

void IspPins::reset(bool asserted) {
   configureOutput(resetSignal, asserted, resetType);
}

void IspSpi::init() {
   Rcc::enableSPI2Clock();
   const Spi master = spi();
   clockHz_ = master.getClockFrequency();
   master.disable();
   master.configure(prescalerOf(shift_), Spi::Mode::Master, Spi::ClockPolarity::Low, Spi::ClockPhase::OneEdge,
                    Spi::DataSize::Bits8, Spi::FirstBit::Msb, Spi::NssMode::Soft);
   master.enable();
}

void IspSpi::fini() {
   spi().disable();
}

void IspSpi::setShift(uint8_t shift) {
   shift_ = shift;
   const Spi master = spi();
   master.disable();
   master.setBaudRatePrescaler(prescalerOf(shift));
   master.enable();
}

bool IspSpi::transfer(IspPins &, const uint8_t *out, uint8_t *in, uint32_t bytes) {
   const Spi master = spi();
   master.clearOverrun();
   // SCK is low in both: the SPI clock idles low (mode 0).
   Mosi::setConfig<spiOutput>();
   Sck::setConfig<spiOutput>();
   bool ok = true;
   for(uint32_t i = 0; i < bytes; ++i) {
      const auto value = master.transfer(out[i]);
      if(!value) {
         ok = false;
         break;
      }
      in[i] = static_cast<uint8_t>(*value);
   }
   // RXNE comes with the last SCK edge, but BSY can stay set a bit longer.
   ok = master.waitUntilIdle() && ok;
   Sck::clear();
   Sck::setConfig<gpioOutput>();
   Mosi::setConfig<gpioOutput>();
   return ok;
}

} // namespace board
