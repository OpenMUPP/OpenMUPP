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
// DMA budget: one transfer took 80 to 210 ns on a dongle. The burst must
// end within the shortest bit (620 ns). A burst of 5 words did not, and a
// sample transfer in the write bits delayed the burst past the update
// event (a 1 bit then kept the low time of the bit before). Thus only the
// read bits have sample transfers.
//
// A read: in the turnaround after bit readFrom-1, the code switches SWDIO
// to open drain and enables the sample request (a window of about 1 us,
// timed by the cycle counter: the timers run at the core clock). Then the
// sample channel copies the input port (IDR) to RAM in each read bit, at
// oneWireSampleNs plus the DMA time. The code reads no timer or DMA
// register during the frame.

static_assert(oneWireTimerOf(current.debug.tmsOut), "the SWDIO output pin has no one-wire timer");
constexpr OneWireTimer oneWire = *oneWireTimerOf(current.debug.tmsOut);

using OneWireOut = FastPinOf<current.debug.tmsOut>;
using OneWireIn = FastPinOf<current.debug.tmsIn>;

constexpr uint8_t afPushPull =
hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
constexpr uint8_t afOpenDrain =
hw::gpio_f1::pinConfig(GpioPin::Mode::Alternate, GpioPin::OutputType::OpenDrain, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
// Input with pull: ODR high selects the pull-up.
constexpr uint8_t releasedInput = hw::gpio_f1::pinConfig(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);

// The burst: ARR, RCR (TIM1) or a reserved word (TIM3), then CCR1 up to
// the CCR of the output channel. Register index of ARR: 11.
constexpr uint32_t arrIndex = 11;
constexpr uint32_t burstWords = 2 + oneWire.channel;
// The read bits, and spare words: the request goes off close to the
// compare of the end period, which can still give a transfer.
constexpr uint32_t sampleWords = 32 + 2;

// Bits 1 to count-1 and two end entries. Bit 0 goes to the registers.
std::array<uint16_t, (rvswd::oneWireFrameBits + 1) * burstWords> table{};

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

// Words: the GPIO registers allow only word access (RM0008 9.2).
std::array<uint32_t, sampleWords> sampleBuffer{};

TIM_TypeDef &timer() {
   return oneWire.timer == 1 ? *TIM1 : *TIM3;
}

Dma::Channel updateDma() {
   return { DMA1, oneWire.updateDma };
}
Dma::Channel sampleDma() {
   return { DMA1, oneWire.sampleDma };
}

constexpr uint32_t sampleRequest = TIM_DIER_CC1DE << (oneWire.sampleChannel - 1);
constexpr uint32_t sampleFlag = TIM_SR_CC1IF << (oneWire.sampleChannel - 1);

volatile uint32_t &ccr(TIM_TypeDef &tim, uint32_t channel) {
   return (&tim.CCR1)[channel - 1];
}

// ARR, the low time and the sample compare of a bit, as a burst entry
void fillEntry(uint16_t *entry, uint32_t period, uint32_t low) {
   entry[0] = static_cast<uint16_t>(period - 1);
   entry[1] = 0;
   for(uint32_t channel = 1; channel <= oneWire.channel; ++channel) {
      entry[1 + channel] = channel == oneWire.channel       ? static_cast<uint16_t>(low) :
                           channel == oneWire.sampleChannel ? ticks.sample :
                                                              0;
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

void resetTimer() {
   if(oneWire.timer == 1) {
      RCC->APB2RSTR |= RCC_APB2RSTR_TIM1RST;
      RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM1RST;
   } else {
      RCC->APB1RSTR |= RCC_APB1RSTR_TIM3RST;
      RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM3RST;
   }
}

void stop(TIM_TypeDef &tim) {
   tim.CR1 &= ~TIM_CR1_CEN;
   tim.DIER &= ~sampleRequest;
   updateDma().disable();
   sampleDma().disable();
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
   ticks = { ticksOf(rvswd::oneWireShortLowNs), ticksOf(rvswd::oneWireLongLowNs),
             ticksOf(rvswd::oneWireHighNs),     ticksOf(rvswd::oneWireTurnaroundHighNs),
             ticksOf(rvswd::oneWireReadBitNs),  ticksOf(rvswd::oneWireSampleNs),
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
   update.configure(const_cast<uint32_t *>(&tim.DMAR), table.data(), Dma::DataSize::Size16Bits,
                    Dma::DataSize::Size16Bits, Dma::Direction::MemoryToPeripheral, true, false);
   update.setPriority(Dma::ChannelPriority::very_high);
   const Dma::Channel samples = sampleDma();
   samples.disable();
   samples.configure(const_cast<uint32_t *>(&OneWireIn::port()->IDR), sampleBuffer.data(), Dma::DataSize::Size32Bits,
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
   // The end: the line high (or released) after the last bit. A DMA
   // request without a transfer stays pending, and the next frame would
   // take it as soon as it enables the channel (and lose bit 0). Thus the
   // second end entry serves the update event at the end of the last bit,
   // and the end period (0.9 ms) is long enough to stop the counter
   // before the next update event.
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);
   fillEntry(&table[entries++ * burstWords], 0x1'0000, 0);

   // The times from the counter start, in ticks (= core cycles)
   auto period = [&](uint32_t i) -> uint32_t { return 1u + (i == 0 ? first[0] : table[(i - 1) * burstWords]); };
   const bool reads = readFrom < count;
   uint32_t switchAt = ticks.lead;
   uint32_t endAt = ticks.lead;
   for(uint32_t i = 0; i < count; ++i) {
      if(i + 1 == readFrom) {
         // After the rising edge of the bit before the read bits
         switchAt =
         endAt + (i == 0 ? first[1 + oneWire.channel] : table[(i - 1) * burstWords + 1 + oneWire.channel]) + ticks.high / 2u;
      }
      endAt += period(i);
   }
   // In the end period, before its sample compare
   endAt += ticks.sample / 2u;

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
   tim.SR = 0;
   OneWireOut::setConfig<afPushPull>();
   const uint32_t start = cycleCount();
   tim.CR1 |= TIM_CR1_CEN;
   if(reads) {
      while(cycleCount() - start < switchAt) {
      }
      OneWireOut::setConfig<afOpenDrain>();
      // The compare of bit readFrom-1 is before this point.
      tim.SR = ~sampleFlag;
      tim.DIER |= sampleRequest;
   }
   while(cycleCount() - start < endAt) {
   }
   // No request is on before bit readFrom, so the first words are the read
   // bits. A transfer after them (the end period) does not matter.
   tim.DIER &= ~sampleRequest;
   // The burst of the update event at the end of the last bit ends some
   // DMA times later. No bit is left, so polling cannot delay one.
   bool done = false;
   const uint32_t end = cycleCount();
   while(!(done = update.isTransferCompleteInterruptFlagSet()) && cycleCount() - end < ticks.readBit) {
   }
   tim.CR1 &= ~TIM_CR1_CEN;
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
