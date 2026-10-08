#include "board/BoardJtag.h"

#include <Afio.h>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Spi.h>

// SPI and DMA of the JTAG fast path (board/BoardJtag.h). RM0008 sections
// 13 (DMA) and 25 (SPI).
//
// SPI1 is a slave on the TCK link, so it also counts the TCK edges of the
// bit-bang engine and of SWD. Each transfer resets SPI1 through the RCC:
// it then starts at bit 0 of its first byte.

namespace board {

namespace {

using hw::Spi;

// On a board without the fast path, nothing calls this code (BoardJtag is
// the bit-bang port), and the linker removes it. The pins then fall back
// to TCK, so the code compiles.
constexpr Pin tckLink = current.jtagFastPath ? current.jtagFastPath->tckLink : current.debug.tck;

using Tck = FastPinOf<current.debug.tck>;
using Tdi = FastPinOf<current.debug.tdi>;
using Tms = FastPinOf<current.debug.tmsOut>;

constexpr uint8_t gpioOutput =
hw::gpio_f1::pinConfig(hw::GpioPin::Mode::Output, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::VeryHigh);
constexpr uint8_t spiOutput =
hw::gpio_f1::pinConfig(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::VeryHigh);

Spi spiMaster() {
   return Spi{ SPI2 };
}
Spi spiSlave() {
   return Spi{ SPI1 };
}

Dma::Channel tmsChannel() {
   return { DMA1, 3 };
}
Dma::Channel tdoChannel() {
   return { DMA1, 4 };
}
Dma::Channel tdiChannel() {
   return { DMA1, 5 };
}

constexpr Spi::BaudRatePrescaler prescalerOf(uint8_t shift) {
   return static_cast<Spi::BaudRatePrescaler>(static_cast<uint32_t>(shift - 1u) << SPI_CR1_BR_Pos);
}

void configureChannel(const Dma::Channel &channel, volatile uint32_t &dr, Dma::Direction direction, Dma::ChannelPriority priority) {
   channel.disable();
   channel.configure(const_cast<uint32_t *>(&dr), nullptr, Dma::DataSize::Size8Bits, Dma::DataSize::Size8Bits, direction, true, false);
   channel.setPriority(priority);
   channel.disableAllInterrupts();
}

void start(const Dma::Channel &channel, const void *memory, uint32_t bytes) {
   channel.disable();
   channel.clearGlobalInterruptFlag();
   channel.setMemoryAddress(reinterpret_cast<uintptr_t>(memory));
   channel.setTransferCount(static_cast<uint16_t>(bytes));
   channel.enable();
}

// SPI1 slave: clocked by TCK on the link, TMS on MISO. SSM with SSI = 0:
// the slave is always selected.
void startSlave() {
   RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
   RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
   const Spi slave = spiSlave();
   slave.configure(Spi::BaudRatePrescaler::Div2, Spi::Mode::Slave, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                   Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft);
   SPI1->CR1 &= ~SPI_CR1_SSI;
   SPI1->CR2 = SPI_CR2_TXDMAEN;
   slave.enable();
}

// The SPI clocks the pins only in their alternate function. TCK is high
// in both: the SPI clock idles high. The change of TDI and TMS does not
// clock the TAP: TCK does not change.
void pinsToSpi() {
   Tdi::setConfig<spiOutput>();
   Tms::setConfig<spiOutput>();
   Tck::setConfig<spiOutput>();
}

void pinsToGpio() {
   Tck::set();
   Tck::setConfig<gpioOutput>();
   Tdi::setConfig<gpioOutput>();
   Tms::setConfig<gpioOutput>();
}

} // namespace

void JtagSpi::init() {
   Rcc::enableSPI1Clock();
   Rcc::enableSPI2Clock();
   Rcc::enableDMA1Clock();
   // Needs SWJ without JTAG-DP: board::swjConfig() gives it, PB3 and PB4
   // are board pins.
   hw::Afio::remapSpi1(true);
   gpioPin(tckLink).configure(hw::GpioPin::Mode::Input);
   clockHz_ = Rcc::getAPBClock();

   const Spi master = spiMaster();
   master.disable();
   master.configure(prescalerOf(shift_), Spi::Mode::Master, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                    Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft);
   SPI2->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
   master.enable();

   // A late TMS byte gives a wrong TMS bit (the slave has no underrun flag),
   // a late TDO read loses a TDO byte. A late TDI byte only stops the
   // clock. At equal priority, the lower channel wins.
   configureChannel(tmsChannel(), SPI1->DR, Dma::Direction::MemoryToPeripheral, Dma::ChannelPriority::very_high);
   configureChannel(tdoChannel(), SPI2->DR, Dma::Direction::PeripheralToMemory, Dma::ChannelPriority::very_high);
   configureChannel(tdiChannel(), SPI2->DR, Dma::Direction::MemoryToPeripheral, Dma::ChannelPriority::high);
}

void JtagSpi::fini() {
   for(const auto &channel : { tmsChannel(), tdoChannel(), tdiChannel() }) {
      channel.disable();
   }
   spiSlave().disable();
   spiMaster().disable();
}

void JtagSpi::setShift(uint8_t shift) {
   shift_ = shift;
   const Spi master = spiMaster();
   master.disable();
   master.setBaudRatePrescaler(prescalerOf(shift));
   master.enable();
}

bool JtagSpi::transfer(JtagPins &pins, const uint8_t *tdi, const uint8_t *tms, uint8_t *tdo, uint32_t bytes) {
   const Dma::Channel tmsDma = tmsChannel();
   const Dma::Channel tdoDma = tdoChannel();
   const Dma::Channel tdiDma = tdiChannel();
   // Twice the time of the bits, and 100 us for the setup
   const uint32_t cyclesPerBit = (pins.coreHz() / clockHz_) << shift_;
   const uint32_t timeout = bytes * 8u * cyclesPerBit * 2u + pins.coreHz() / 10'000u;

   startSlave();
   start(tmsDma, tms, bytes);
   // The slave must have its first TMS byte before the first TCK edge.
   uint32_t begin = pins.now();
   while(tmsDma.getDataCount() == bytes) {
      if(pins.now() - begin >= timeout) {
         tmsDma.disable();
         spiSlave().disable();
         return false;
      }
   }

   const Spi master = spiMaster();
   master.clearOverrun();
   start(tdoDma, tdo, bytes);
   pinsToSpi();
   // The first TDI byte starts the clock.
   start(tdiDma, tdi, bytes);

   bool ok = true;
   begin = pins.now();
   while(!tdoDma.isTransferCompleteInterruptFlagSet()) {
      if(pins.now() - begin >= timeout) {
         ok = false;
         break;
      }
   }
   // The last TDO byte came in with the last rising edge: TCK is high.
   pinsToGpio();
   for(const auto &channel : { tmsDma, tdoDma, tdiDma }) {
      channel.disable();
      channel.clearGlobalInterruptFlag();
   }
   spiSlave().disable();
   return ok;
}

} // namespace board
