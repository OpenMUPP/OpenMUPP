// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardRvSwd.h"

#include <array>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Timer.h>

// The one-wire timer of the RISC-V debug wire engine on the CH32V307:
// TIM10 and DMA2 (CH32FV2x_V3xRM chapters 11 and 14). The timer, the DMA
// and the GPIO are the STM32F1 blocks with WCH register names, so the frame
// works as on the STM32F1 (BoardRvSwd.cpp has the details). The
// differences:
//
// - TIM10 is an advanced timer: its output needs BDTR.MOE, also on a
//   channel without a complementary output.
// - The output is TIM10_CH4 (PC11, default map), so the burst has 6 words:
//   ATRLR, RPTCR, then CH1CVR to CH4CVR. On the STM32F1 a burst of 5 words
//   did not end within the shortest bit (620 ns) at 72 MHz. The CH32V307
//   bus runs at 144 MHz. The frames work with a CH32V003 (docs/wch-link.md),
//   but the wire times are not measured.
// - The requests are on DMA2: TIM10_UP on channel 11, TIM10_CH1 (the
//   sample) on channel 8 (RM table 11-3).
//
// TIM10 runs at the core clock: PCLK2 is HCLK (initialize_hardware.cpp), and
// the timer clock is PCLK2 when the APB2 prescaler is 1 (RM clock tree).
// During a Vref measurement PCLK2 is HCLK / 2, and the timer clock is twice
// PCLK2, so it stays the same. The timer ticks are core cycles.
template class rvswd::RvSwdEngine<board::RvSwdPins>;

namespace board {

namespace {

using hw::GpioPin;

static_assert(oneWireTimerOf(current.family, current.debug.tmsOut), "the SWDIO output pin has no one-wire timer");
constexpr OneWireTimer oneWire = *oneWireTimerOf(current.family, current.debug.tmsOut);
static_assert(oneWire.timer == 10 && !oneWire.complementary && !oneWire.partialRemap,
              "the CH32V307 engine supports TIM10_CHx (default map) only");

using OneWireOut = FastPinOf<current.debug.tmsOut>;
using OneWireIn = FastPinOf<current.debug.tmsIn>;

// The output register first, so the pin starts at its level.
void configureOutput(Pin pin, bool level) {
   const GpioPin gpio = gpioPin(pin);
   gpio.write(level);
   gpio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
}

constexpr uint8_t afPushPull =
hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
constexpr uint8_t afOpenDrain =
hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::OpenDrain, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
// Input with pull: ODR high selects the pull-up.
constexpr uint8_t releasedInput = hw::gpio_f1::pinConfig(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);

// The burst: ATRLR, RPTCR, then CH1CVR up to the CVR of the output
// channel. Register index of ATRLR: 11. DMACFGR: DBL in bits 12:8.
constexpr uint32_t arrIndex = 11;
constexpr uint32_t dblShift = 8;
constexpr uint32_t burstWords = 2 + oneWire.channel;
// The read bits, and spare words: the request goes off close to the
// compare of the end period, which can still give a transfer.
constexpr uint32_t sampleWords = 32 + 2;

// Bits 1 to count-1 and two end entries. Bit 0 goes to the registers.
std::array<uint16_t, (rvswd::oneWireFrameBits + 1) * burstWords> table{};

rvswd::OneWireTicks ticks{};
// False below rvswd::oneWireMinClockHz (for example on the HSI after a
// crystal failure): the frames then fail, and send no bits.
bool usable = false;

// Words: the sample channel copies the 32-bit INDR.
std::array<uint32_t, sampleWords> sampleBuffer{};

TIM_TypeDef &timer() {
   return *TIM10;
}

Dma::Channel updateDma() {
   return { DMA2, oneWire.updateDma };
}
Dma::Channel sampleDma() {
   return { DMA2, oneWire.sampleDma };
}

constexpr uint32_t sampleRequest = TIM_CC1DE << (oneWire.sampleChannel - 1);
constexpr uint32_t sampleFlag = TIM_CC1IF << (oneWire.sampleChannel - 1);

volatile uint32_t &ccr(TIM_TypeDef &tim, uint32_t channel) {
   return (&tim.CH1CVR)[channel - 1];
}

// ATRLR, the low time and the sample compare of a bit, as a burst entry
void fillEntry(uint16_t *entry, uint32_t period, uint32_t low) {
   entry[0] = static_cast<uint16_t>(period - 1);
   entry[1] = 0;
   for(uint32_t channel = 1; channel <= oneWire.channel; ++channel) {
      entry[1 + channel] = channel == oneWire.channel       ? static_cast<uint16_t>(low) :
                           channel == oneWire.sampleChannel ? static_cast<uint16_t>(ticks.sample) :
                                                              0;
   }
}

void fillBit(uint16_t *entry, uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t i) {
   const rvswd::OneWireBit bit = rvswd::oneWireBit(ticks, bits, count, readFrom, i);
   fillEntry(entry, bit.period, bit.low);
}

void stop(TIM_TypeDef &tim) {
   tim.CTLR1 &= ~TIM_CEN;
   tim.DMAINTENR &= ~sampleRequest;
   updateDma().disable();
   sampleDma().disable();
}

void initOneWire() {
   Rcc::enableTIM10Clock();
   Rcc::enableDMA2Clock();
   ticks = rvswd::oneWireTicks(SystemCoreClock);
   usable = SystemCoreClock >= rvswd::oneWireMinClockHz;

   TIM_TypeDef &tim = timer();
   const hw::Timer t(&tim);
   tim.CTLR1 = TIM_ARPE | TIM_URS;
   tim.PSC = 0;
   const auto out = static_cast<hw::Timer::Channel>(oneWire.channel - 1);
   const auto sample = static_cast<hw::Timer::Channel>(oneWire.sampleChannel - 1);
   t.setOcMode(out, hw::Timer::OcMode::Pwm1);
   t.enableOcPreload(out);
   // Frozen: the compare sets the flag and the DMA request, no output.
   t.setOcMode(sample, hw::Timer::OcMode::Frozen);
   t.enableOcPreload(sample);
   // Inverted output: OCREF high (CNT < CVR) is the low level.
   tim.CCER = (TIM_CC1E | TIM_CC1P) << ((oneWire.channel - 1) * 4);
   t.enableMainOutput();
   tim.DMACFGR = (burstWords - 1) << dblShift | arrIndex;
   tim.DMAINTENR = TIM_UDE;

   const Dma::Channel update = updateDma();
   update.disable();
   update.configure(const_cast<uint32_t *>(&tim.DMAADR), table.data(), Dma::DataSize::Size16Bits,
                    Dma::DataSize::Size16Bits, Dma::Direction::MemoryToPeripheral, true, false);
   update.setPriority(Dma::ChannelPriority::very_high);
   const Dma::Channel samples = sampleDma();
   samples.disable();
   samples.configure(const_cast<uint32_t *>(&OneWireIn::port()->INDR), sampleBuffer.data(), Dma::DataSize::Size32Bits,
                     Dma::DataSize::Size32Bits, Dma::Direction::PeripheralToMemory, true, false);
   samples.setPriority(Dma::ChannelPriority::high);
}

} // namespace

void RvSwdPins::init() {
   const DebugPins &pins = current.debug;
   // An open SWDIO reads 1.
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   configureOutput(pins.tck, false);
   configureOutput(pins.tmsOut, true);
   initOneWire();
}

void RvSwdPins::fini() {
   // The timer keeps its output and its DMA request enabled after a frame.
   // The JTAG fast path uses the same pin (PC11, SPI3 MISO), so the reset
   // turns the output off.
   stop(timer());
   Rcc::resetTIM10();
   SwdPins::fini();
}

bool RvSwdPins::oneWireFrame(uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t &samples) {
   TIM_TypeDef &tim = timer();
   stop(tim);
   if(!usable) {
      OneWireOut::set();
      OneWireOut::setConfig<releasedInput>();
      return false;
   }
   // The lead-in: CVR 0 keeps the line high while SWDIO becomes the timer
   // output, so the first edge comes from the timer.
   tim.ATRLR = ticks.lead - 1u;
   ccr(tim, oneWire.channel) = 0;
   ccr(tim, oneWire.sampleChannel) = ticks.sample;
   tim.CNT = 0;
   // URS: the update generation loads the registers without a DMA request.
   tim.SWEVGR = TIM_UG;
   std::array<uint16_t, burstWords> first{};
   fillBit(first.data(), bits, count, readFrom, 0);
   tim.ATRLR = first[0];
   ccr(tim, oneWire.channel) = first[1 + oneWire.channel];

   uint32_t entries = 0;
   for(uint32_t i = 1; i < count; ++i) {
      fillBit(&table[entries++ * burstWords], bits, count, readFrom, i);
   }
   // The end: see BoardRvSwd.cpp. Two end entries, and an end period that
   // is long enough to stop the counter before the next update event.
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);

   // The times from the counter start, in ticks (= core cycles)
   const rvswd::OneWireTimes times = rvswd::oneWireTimes(ticks, bits, count, readFrom);
   const bool reads = readFrom < count;

   const Dma::Channel update = updateDma();
   update.setMemoryAddress(reinterpret_cast<uint32_t>(table.data()));
   update.setTransferCount(static_cast<uint16_t>(entries * burstWords));
   update.clearGlobalInterruptFlag();
   update.enable();
   const Dma::Channel sample = sampleDma();
   if(reads) {
      sample.setMemoryAddress(reinterpret_cast<uint32_t>(sampleBuffer.data()));
      sample.setTransferCount(sampleWords);
      sample.clearGlobalInterruptFlag();
      sample.enable();
   }
   tim.INTFR = 0;
   OneWireOut::setConfig<afPushPull>();
   const uint32_t start = cycleCount();
   tim.CTLR1 |= TIM_CEN;
   if(reads) {
      while(cycleCount() - start < times.switchAt) {
      }
      OneWireOut::setConfig<afOpenDrain>();
      // The compare of bit readFrom-1 is before this point.
      tim.INTFR = ~sampleFlag;
      tim.DMAINTENR |= sampleRequest;
   }
   while(cycleCount() - start < times.endAt) {
   }
   // No request is on before bit readFrom, so the first words are the read
   // bits. A transfer after them (the end period) does not matter.
   tim.DMAINTENR &= ~sampleRequest;
   // The burst of the update event at the end of the last bit ends some
   // DMA times later. No bit is left, so polling cannot delay one.
   bool done = false;
   const uint32_t end = cycleCount();
   while(!(done = update.isTransferCompleteInterruptFlagSet()) && cycleCount() - end < ticks.readBit) {
   }
   tim.CTLR1 &= ~TIM_CEN;
   const uint32_t taken = sampleWords - sample.getDataCount();
   const bool ok = done && (!reads || taken >= count - readFrom);
   if(reads || !ok) {
      OneWireOut::set();
      OneWireOut::setConfig<releasedInput>();
   } else {
      OneWireOut::write(true);
      OneWireOut::setOutput();
   }
   stop(tim);
   if(!ok) {
      // A DMA request can be pending in the timer: a reset clears it.
      Rcc::resetTIM10();
      initOneWire();
      return false;
   }
   samples = 0;
   for(uint32_t i = 0; reads && i < count - readFrom; ++i) {
      samples = samples << 1 | ((sampleBuffer[i] & OneWireIn::mask) != 0 ? 1u : 0u);
   }
   return true;
}

} // namespace board
