// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardRvSwd.h"

#include <array>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Timer.h>

// The one-wire timer of the RISC-V debug wire engine on the STM32H5:
// TIM3 and GPDMA1 (RM0481 sections 16 and 44). The frame works as on the
// STM32F1 (BoardRvSwd.cpp has the details); this file has the differences:
//
// - The burst source of the timer is DCR.DBSS (update). 0 is reserved.
// - A GPDMA channel counts bytes, and gets its request from REQSEL.
// - GPIO v2: the pin changes between output, alternate function and input
//   in MODER, and to open drain in OTYPER. PUPDR keeps the pull-up of
//   init() in all modes.
//
// TIM3 runs at the core clock: PCLK1 is HCLK (system-stm32 RccH5), so the
// timer ticks are core cycles. The engine compiles with -O2 (CMakeLists.txt).
template class rvswd::RvSwdEngine<board::RvSwdPins>;

namespace board {

namespace {

using hw::GpioPin;

static_assert(oneWireTimerOf(current.family, current.debug.tmsOut), "the SWDIO output pin has no one-wire timer");
constexpr OneWireTimer oneWire = *oneWireTimerOf(current.family, current.debug.tmsOut);
static_assert(oneWire.timer == 3 && !oneWire.complementary, "the STM32H5 engine supports TIM3_CHx only");

using OneWireOut = FastPinOf<current.debug.tmsOut>;
using OneWireIn = FastPinOf<current.debug.tmsIn>;

// TIM3_CH1 on PA6 (DS14540 table 14)
constexpr auto timerFunction = GpioPin::AlternateFunction::AF2;

// The burst: ARR, a reserved word (RCR of TIM1), then CCR1 up to the CCR of
// the output channel. Register index of ARR: 11.
constexpr uint32_t arrIndex = 11;
constexpr uint32_t burstWords = 2 + oneWire.channel;
// The read bits, and spare words, as on the STM32F1
constexpr uint32_t sampleWords = 32 + 2;

std::array<uint16_t, (rvswd::oneWireFrameBits + 1) * burstWords> table{};

rvswd::OneWireTicks ticks{};
bool usable = false;

std::array<uint32_t, sampleWords> sampleBuffer{};

TIM_TypeDef &timer() {
   return *TIM3;
}

Dma::Channel updateDma() {
   return { GPDMA1, oneWire.updateDma };
}
Dma::Channel sampleDma() {
   return { GPDMA1, oneWire.sampleDma };
}

constexpr Dma::Config updateConfig{ .direction = Dma::Direction::MemoryToPeripheral,
                                    .request = Dma::Request::tim3_upd,
                                    .sourceSize = Dma::DataSize::Size16Bits,
                                    .destinationSize = Dma::DataSize::Size16Bits,
                                    .sourceIncrement = true };
// The requests of TIM3_CH1 to TIM3_CH4 follow each other (RM0481 table 142)
constexpr Dma::Config sampleConfig{ .direction = Dma::Direction::PeripheralToMemory,
                                    .request = static_cast<Dma::Request>(static_cast<uint8_t>(Dma::Request::tim3_cc1) +
                                                                         oneWire.sampleChannel - 1),
                                    .sourceSize = Dma::DataSize::Size32Bits,
                                    .destinationSize = Dma::DataSize::Size32Bits,
                                    .destinationIncrement = true };

constexpr uint32_t sampleRequest = TIM_DIER_CC1DE << (oneWire.sampleChannel - 1);
constexpr uint32_t sampleFlag = TIM_SR_CC1IF << (oneWire.sampleChannel - 1);

volatile uint32_t &ccr(TIM_TypeDef &tim, uint32_t channel) {
   return (&tim.CCR1)[channel - 1];
}

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

void resetTimer() {
   RCC->APB1LRSTR |= RCC_APB1LRSTR_TIM3RST;
   RCC->APB1LRSTR &= ~RCC_APB1LRSTR_TIM3RST;
}

void stop(TIM_TypeDef &tim) {
   tim.CR1 &= ~TIM_CR1_CEN;
   tim.DIER &= ~sampleRequest;
   updateDma().abort();
   sampleDma().abort();
}

void setOpenDrain(bool openDrain) {
   if(openDrain) {
      OneWireOut::port()->OTYPER |= OneWireOut::mask;
   } else {
      OneWireOut::port()->OTYPER &= ~OneWireOut::mask;
   }
}

// After a frame: released (input with the pull-up) or driven high. ODR
// high first, so a later output starts high.
void endLine(bool release) {
   OneWireOut::set();
   if(release) {
      OneWireOut::setInput();
   } else {
      OneWireOut::setOutput();
   }
   setOpenDrain(false);
}

void initOneWire() {
   Rcc::enableTIM3Clock();
   Rcc::enableGPDMA1Clock();
   ticks = rvswd::oneWireTicks(SystemCoreClock);
   usable = SystemCoreClock >= rvswd::oneWireMinClockHz;

   TIM_TypeDef &tim = timer();
   const hw::Timer t(&tim);
   tim.CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
   tim.PSC = 0;
   const auto out = static_cast<hw::Timer::Channel>(oneWire.channel - 1);
   const auto sample = static_cast<hw::Timer::Channel>(oneWire.sampleChannel - 1);
   t.setOcMode(out, hw::Timer::OcMode::Pwm1);
   t.enableOcPreload(out);
   t.setOcMode(sample, hw::Timer::OcMode::Frozen);
   t.enableOcPreload(sample);
   // Inverted output: OCREF high (CNT < CCR) is the low level.
   tim.CCER = (TIM_CCER_CC1E | TIM_CCER_CC1P) << ((oneWire.channel - 1) * 4);
   tim.DCR = 1u << TIM_DCR_DBSS_Pos | (burstWords - 1) << TIM_DCR_DBL_Pos | arrIndex;
   tim.DIER = TIM_DIER_UDE;
}

} // namespace

void RvSwdPins::init() {
   const DebugPins &pins = current.debug;
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   const GpioPin tck = gpioPin(pins.tck);
   tck.write(false);
   tck.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
   // The pull-up holds a released SWDIO high (SwdPins::swdioRelease()). The
   // AF number counts only in the alternate function mode of a frame; the
   // JTAG fast path sets its own in JtagSpi::init().
   const GpioPin swdio = gpioPin(pins.tmsOut);
   swdio.write(true);
   swdio.setAlternateFunction(timerFunction);
   swdio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::Up, GpioPin::Speed::VeryHigh);
   initOneWire();
}

void RvSwdPins::fini() {
   stop(timer());
   resetTimer();
   SwdPins::fini();
}

bool RvSwdPins::oneWireFrame(uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t &samples) {
   TIM_TypeDef &tim = timer();
   stop(tim);
   if(!usable) {
      endLine(true);
      return false;
   }
   // The lead-in: CCR 0 keeps the line high while SWDIO becomes the timer
   // output, so the first edge comes from the timer.
   tim.ARR = ticks.lead - 1u;
   ccr(tim, oneWire.channel) = 0;
   ccr(tim, oneWire.sampleChannel) = ticks.sample;
   tim.CNT = 0;
   // URS: the update generation loads the registers without a DMA request.
   tim.EGR = TIM_EGR_UG;
   std::array<uint16_t, burstWords> first{};
   fillBit(first.data(), bits, count, readFrom, 0);
   tim.ARR = first[0];
   ccr(tim, oneWire.channel) = first[1 + oneWire.channel];

   uint32_t entries = 0;
   for(uint32_t i = 1; i < count; ++i) {
      fillBit(&table[entries++ * burstWords], bits, count, readFrom, i);
   }
   // Two end entries, as on the STM32F1. The end period is 262 us at
   // 250 MHz: long enough to stop the counter before its update event.
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);

   const rvswd::OneWireTimes times = rvswd::oneWireTimes(ticks, bits, count, readFrom);
   const bool reads = readFrom < count;

   const Dma::Channel update = updateDma();
   update.start(updateConfig, table.data(), const_cast<uint32_t *>(&tim.DMAR),
                static_cast<uint16_t>(entries * burstWords * sizeof(uint16_t)), Dma::Priority::very_high);
   const Dma::Channel sample = sampleDma();
   if(reads) {
      sample.start(sampleConfig, const_cast<uint32_t *>(&OneWireIn::port()->IDR), sampleBuffer.data(),
                   sizeof(sampleBuffer), Dma::Priority::high);
   }
   tim.SR = 0;
   OneWireOut::setMode<GpioPin::Mode::Alternate>();
   const uint32_t start = cycleCount();
   tim.CR1 |= TIM_CR1_CEN;
   if(reads) {
      while(cycleCount() - start < times.switchAt) {
      }
      setOpenDrain(true);
      tim.SR = ~sampleFlag;
      tim.DIER |= sampleRequest;
   }
   while(cycleCount() - start < times.endAt) {
   }
   tim.DIER &= ~sampleRequest;
   bool done = false;
   const uint32_t end = cycleCount();
   while(!(done = update.isTransferCompleteFlagSet()) && cycleCount() - end < ticks.readBit) {
   }
   tim.CR1 &= ~TIM_CR1_CEN;
   const uint32_t taken = sampleWords - sample.remainingBytes() / sizeof(uint32_t);
   const bool ok = done && (!reads || taken >= count - readFrom);
   endLine(reads || !ok);
   stop(tim);
   if(!ok) {
      // A DMA request can be pending in the timer: a reset clears it.
      resetTimer();
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
