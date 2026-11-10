// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardJtag.h"

#include <Afio.h>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Spi.h>

// SPI and DMA of the JTAG fast path on the CH32V307 (board/BoardJtag.h).
// CH32FV2x_V3xRM chapters 11 (DMA) and 20 (SPI). The SPI and DMA are the
// STM32F1 blocks with WCH register names, so the code follows the STM32F1
// version (BoardJtagFast.cpp). Differences: SPI3 (remapped to PC10 and
// PC11) is the slave, and its TX request is on DMA2.
//
// SPI3 is a slave on the TCK link, so it also counts the TCK edges of the
// bit-bang engine and of SWD. Each transfer resets SPI3 through the RCC:
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
   return Spi{ SPI3 };
}

// RM tables 11-2 and 11-3. The one-wire engine (BoardRvSwdV30x.cpp) uses
// DMA2 channels 8 and 11.
Dma::Channel tmsChannel() {
   return { DMA2, 2 }; // SPI3 TX
}
Dma::Channel tdoChannel() {
   return { DMA1, 4 }; // SPI2 RX
}
Dma::Channel tdiChannel() {
   return { DMA1, 5 }; // SPI2 TX
}

constexpr Spi::BaudRatePrescaler prescalerOf(uint8_t shift) {
   return static_cast<Spi::BaudRatePrescaler>(static_cast<uint32_t>(shift - 1u) * SPI_CTLR1_BR_0);
}

void configureChannel(const Dma::Channel &channel, volatile uint16_t &dr, Dma::Direction direction, Dma::ChannelPriority priority) {
   channel.disable();
   channel.configure(const_cast<uint16_t *>(&dr), nullptr, Dma::DataSize::Size8Bits, Dma::DataSize::Size8Bits, direction, true, false);
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

// SPI3 slave: clocked by TCK on the link, TMS on MISO. NssMode::Soft on a
// slave clears SSI: the slave is always selected.
void startSlave() {
   Rcc::resetSPI3();
   const Spi slave = spiSlave();
   slave.configure(Spi::BaudRatePrescaler::Div2, Spi::Mode::Slave, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                   Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft);
   slave.enableTxDma();
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
   Rcc::enableSPI2Clock();
   Rcc::enableSPI3Clock();
   Rcc::enableDMA1Clock();
   Rcc::enableDMA2Clock();
   // SCK on PC10, MISO on PC11. The default map uses PA15, PB3 and PB4
   // (the LEDs and the key of the YD board).
   hw::Afio::remapSpi3(true);
   gpioPin(tckLink).configure(hw::GpioPin::Mode::Input);
   const Spi master = spiMaster();
   clockHz_ = master.getClockFrequency();

   master.disable();
   master.configure(prescalerOf(shift_), Spi::Mode::Master, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                    Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft);
   master.enableRxDma();
   master.enableTxDma();
   master.enable();

   // A late TMS byte gives a wrong TMS bit (the slave has no underrun flag),
   // a late TDO read loses a TDO byte. A late TDI byte only stops the
   // clock. TMS runs on DMA2, in parallel with DMA1. In DMA1, TDO has a
   // higher priority than TDI.
   configureChannel(tmsChannel(), SPI3->DATAR, Dma::Direction::MemoryToPeripheral, Dma::ChannelPriority::very_high);
   configureChannel(tdoChannel(), SPI2->DATAR, Dma::Direction::PeripheralToMemory, Dma::ChannelPriority::very_high);
   configureChannel(tdiChannel(), SPI2->DATAR, Dma::Direction::MemoryToPeripheral, Dma::ChannelPriority::high);
}

void JtagSpi::fini() {
   for(const auto &channel : { tmsChannel(), tdoChannel(), tdiChannel() }) {
      channel.disable();
   }
   const Spi slave = spiSlave();
   const Spi master = spiMaster();
   slave.disable();
   master.disable();
   // A disabled SPI with TXDMAEN still requests DMA (TXE is set).
   slave.disableTxDma();
   master.disableTxDma();
   master.disableRxDma();
   // A disabled SPI3 still owns PC11 (MISO) while it is remapped, and can
   // mask the TIM10_CH4 output of the one-wire engine on the same pin.
   hw::Afio::remapSpi3(false);
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

   // Wait on the cycle counter (not on the bus) for the time of the bits:
   // a DMA register poll takes bus cycles from the three channels.
   begin = pins.now();
   const uint32_t bitCycles = bytes * 8u * cyclesPerBit;
   while(pins.now() - begin < bitCycles) {
   }
   bool ok = true;
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
