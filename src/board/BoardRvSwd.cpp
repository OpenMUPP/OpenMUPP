#include "board/BoardRvSwd.h"

#include <Afio.h>
#include <array>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Timer.h>

// The RISC-V debug wire engine, compiled with -O2 (CMakeLists.txt).
template class rvswd::RvSwdEngine<board::RvSwdPins>;

namespace board {

namespace {

using hw::GpioPin;

// The output register first, so the pin starts at its level.
void configureOutput(Pin pin, bool level) {
   const GpioPin gpio = gpioPin(pin);
   gpio.write(level);
   gpio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
}

// One wire. The timer makes each edge: PWM mode 1 with an inverted output
// gives a low level of CCR ticks, then a high level until ARR. On each
// update event, a DMA burst writes ARR and the CCRs of the next bit (DCR,
// RM0008 15.4.19); the preload registers then take them at the next
// update event. No code runs between two bits.
//
// A read: after the bit before the read bits, the code switches SWDIO to
// open drain (a window of about 1 us) and starts the sample DMA. At
// oneWireSampleNs in each read bit, the sample channel copies the input
// port (IDR) to RAM.

static_assert(oneWireTimerOf(current.debug.tmsOut), "the SWDIO output pin has no one-wire timer");
constexpr OneWireTimer oneWire = *oneWireTimerOf(current.debug.tmsOut);

using OneWireOut = FastPinOf<current.debug.tmsOut>;
using OneWireIn = FastPinOf<current.debug.tmsIn>;

constexpr uint8_t afPushPull = hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::PushPull, GpioPin::Pull::None,
                                                     GpioPin::Speed::VeryHigh);
constexpr uint8_t afOpenDrain = hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::OpenDrain, GpioPin::Pull::None,
                                                      GpioPin::Speed::VeryHigh);
// Input with pull: ODR high selects the pull-up.
constexpr uint8_t releasedInput = hw::gpio_f1::pinConfig(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);

// The burst: ARR, RCR (TIM1) or a reserved word (TIM3), then CCR1 up to
// the CCR of the output channel. Register index of ARR: 11.
constexpr uint32_t arrIndex = 11;
constexpr uint32_t burstWords = 2 + oneWire.channel;

// Bits 1 to count-1 and the end entry. Bit 0 goes to the registers.
std::array<uint16_t, rvswd::oneWireFrameBits * burstWords> table{};
std::array<uint16_t, rvswd::oneWireFrameBits> sampleBuffer{};

struct Ticks {
   uint16_t shortLow;
   uint16_t longLow;
   uint16_t high;
   uint16_t turnaroundHigh;
   uint16_t readBit;
   uint16_t sample;
   uint16_t lead;
};
Ticks ticks{};

TIM_TypeDef &timer() {
   return oneWire.timer == 1 ? *TIM1 : *TIM3;
}

Dma::Channel updateDma() {
   return { DMA1, oneWire.updateDma };
}
Dma::Channel sampleDma() {
   return { DMA1, oneWire.sampleDma };
}

volatile uint32_t &ccr(TIM_TypeDef &tim, uint32_t channel) {
   return (&tim.CCR1)[channel - 1];
}

// ARR, the low time and the sample compare of a bit, as a burst entry
void fillEntry(uint16_t *entry, uint32_t period, uint32_t low) {
   entry[0] = static_cast<uint16_t>(period - 1);
   entry[1] = 0;
   for(uint32_t channel = 1; channel <= oneWire.channel; ++channel) {
      entry[1 + channel] = channel == oneWire.channel         ? static_cast<uint16_t>(low)
                           : channel == oneWire.sampleChannel ? ticks.sample
                                                              : 0;
   }
}

void fillBit(uint16_t *entry, uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t i) {
   if(i >= readFrom) {
      fillEntry(entry, ticks.readBit, ticks.shortLow);
      return;
   }
   const uint32_t low = (bits >> (count - 1 - i) & 1u) != 0 ? ticks.shortLow : ticks.longLow;
   fillEntry(entry, low + (i + 1 == readFrom ? ticks.turnaroundHigh : ticks.high), low);
}

void stop(TIM_TypeDef &tim) {
   tim.CR1 &= ~TIM_CR1_CEN;
   updateDma().disable();
   sampleDma().disable();
   tim.DIER &= ~(TIM_DIER_CC1DE << (oneWire.sampleChannel - 1));
}

// Timer ticks of a time. Both timers run at HCLK: TIM1 on APB2 (HCLK),
// TIM3 on APB1 (HCLK/2, the timer clock is twice that, RM0008 7.2).
uint16_t ticksOf(uint32_t ns) {
   return static_cast<uint16_t>(rvswd::cyclesOfNs(SystemCoreClock, ns));
}

void initOneWire() {
   if(oneWire.timer == 1) {
      Rcc::enableTIM1Clock();
   } else {
      Rcc::enableTIM3Clock();
   }
   Rcc::enableDMA1Clock();
   if(oneWire.partialRemap) {
      hw::Afio::setRemap(AFIO_MAPR_TIM3_REMAP, AFIO_MAPR_TIM3_REMAP_PARTIALREMAP);
   }
   ticks = { ticksOf(rvswd::oneWireShortLowNs),       ticksOf(rvswd::oneWireLongLowNs), ticksOf(rvswd::oneWireHighNs),
             ticksOf(rvswd::oneWireTurnaroundHighNs), ticksOf(rvswd::oneWireReadBitNs), ticksOf(rvswd::oneWireSampleNs),
             ticksOf(rvswd::oneWireLeadNs) };

   TIM_TypeDef &tim = timer();
   const hw::Timer t(&tim);
   tim.CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
   tim.PSC = 0;
   const auto out = static_cast<hw::Timer::Channel>(oneWire.channel - 1);
   const auto sample = static_cast<hw::Timer::Channel>(oneWire.sampleChannel - 1);
   t.setOcMode(out, hw::Timer::OcMode::Pwm1);
   t.enableOcPreload(out);
   // Frozen: the compare sets the flag and the DMA request, no output.
   t.setOcMode(sample, hw::Timer::OcMode::Frozen);
   t.enableOcPreload(sample);
   const uint32_t shift = (oneWire.channel - 1) * 4;
   // Inverted output: OCREF high (CNT < CCR) is the low level.
   tim.CCER = oneWire.complementary ? (TIM_CCER_CC1NE | TIM_CCER_CC1NP) << shift : (TIM_CCER_CC1E | TIM_CCER_CC1P) << shift;
   if(oneWire.complementary) {
      tim.BDTR = TIM_BDTR_MOE;
   }
   tim.DCR = (burstWords - 1) << TIM_DCR_DBL_Pos | arrIndex;
   tim.DIER = TIM_DIER_UDE;

   const Dma::Channel update = updateDma();
   update.disable();
   update.configure(const_cast<uint32_t *>(&tim.DMAR), table.data(), Dma::DataSize::Size16Bits, Dma::DataSize::Size16Bits,
                    Dma::Direction::MemoryToPeripheral, true, false);
   update.setPriority(Dma::ChannelPriority::very_high);
   const Dma::Channel samples = sampleDma();
   samples.disable();
   samples.configure(const_cast<uint32_t *>(&OneWireIn::port()->IDR), sampleBuffer.data(), Dma::DataSize::Size16Bits,
                     Dma::DataSize::Size16Bits, Dma::Direction::PeripheralToMemory, true, false);
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

bool RvSwdPins::oneWireFrame(uint64_t bits, uint32_t count, uint32_t readFrom, uint32_t &samples) {
   TIM_TypeDef &tim = timer();
   stop(tim);
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
   // The end: the line high (or released) after the last bit
   fillEntry(&table[entries++ * burstWords], ticks.high, 0);

   const Dma::Channel update = updateDma();
   update.setMemoryAddress(reinterpret_cast<uint32_t>(table.data()));
   update.setTransferCount(static_cast<uint16_t>(entries * burstWords));
   update.clearGlobalInterruptFlag();
   update.enable();
   const bool reads = readFrom < count;
   const Dma::Channel sample = sampleDma();
   if(reads) {
      sample.setMemoryAddress(reinterpret_cast<uint32_t>(sampleBuffer.data()));
      sample.setTransferCount(static_cast<uint16_t>(count - readFrom));
      sample.clearGlobalInterruptFlag();
      sample.enable();
   }
   tim.SR = 0;
   OneWireOut::setConfig<afPushPull>();
   const uint32_t start = cycleCount();
   tim.CR1 |= TIM_CR1_CEN;

   // A frame takes at most about 60 us.
   const uint32_t timeout = rvswd::cyclesOfNs(SystemCoreClock, 200'000);
   auto waitFor = [&](auto done) {
      while(!done()) {
         if(cycleCount() - start > timeout) {
            return false;
         }
      }
      return true;
   };
   const uint32_t outFlag = TIM_SR_CC1IF << (oneWire.channel - 1);
   bool ok = true;
   if(reads) {
      // The update DMA writes bit readFrom at the start of bit readFrom-1.
      // In a read, that bit is the write bit 0: its long low (860 ns) ends
      // after the burst, so the next compare flag is its rising edge.
      const uint32_t left = (entries - readFrom) * burstWords;
      ok = waitFor([&] { return update.getDataCount() <= left; });
      tim.SR = ~outFlag;
      ok = ok && waitFor([&] { return (tim.SR & outFlag) != 0; });
      OneWireOut::setConfig<afOpenDrain>();
      // The sample of bit readFrom-1 (at 600 ns) is before this edge.
      tim.DIER |= TIM_DIER_CC1DE << (oneWire.sampleChannel - 1);
   }
   // The end entry goes to the preload registers at the start of the last
   // bit; the next update event is the end of the last bit.
   ok = ok && waitFor([&] { return update.isTransferCompleteInterruptFlagSet(); });
   tim.SR = ~TIM_SR_UIF;
   ok = ok && waitFor([&] { return (tim.SR & TIM_SR_UIF) != 0; });
   if(reads || !ok) {
      OneWireOut::set();
      OneWireOut::setConfig<releasedInput>();
   } else {
      OneWireOut::write(true);
      OneWireOut::setOutput();
   }
   stop(tim);
   if(!ok) {
      return false;
   }
   samples = 0;
   for(uint32_t i = 0; reads && i < count - readFrom; ++i) {
      samples = samples << 1 | ((sampleBuffer[i] & OneWireIn::mask) != 0 ? 1u : 0u);
   }
   return true;
}

} // namespace board
