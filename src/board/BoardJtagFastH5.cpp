#include "board/BoardJtag.h"

#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
#include <peripherals/Spi.h>

// SPI v2 and GPDMA of the JTAG fast path on the STM32H5 (board/BoardJtag.h).
// RM0481 sections 16 (GPDMA) and 58 (SPI). The STM32F1 version is
// BoardJtagFast.cpp.
//
// SPI1 is a slave on the TCK link, so it also counts the TCK edges of the
// bit-bang engine and of SWD. Each transfer resets SPI1 through the RCC:
// it then starts at bit 0 of its first byte.

namespace board {

namespace {

using hw::GpioPin;
using hw::Spi;

// On a board without the fast path, nothing calls this code (BoardJtag is
// the bit-bang port), and the linker removes it. The pins then fall back
// to TCK, so the code compiles.
constexpr Pin tckLink = current.jtagFastPath ? current.jtagFastPath->tckLink : current.debug.tck;

using Tck = FastPinOf<current.debug.tck>;
using Tdi = FastPinOf<current.debug.tdi>;
using Tdo = FastPinOf<current.debug.tdo>;
using Tms = FastPinOf<current.debug.tmsOut>;

// SPI1 and SPI2 are AF5 on all pins of the fast path (DS14540 table 16)
constexpr auto spiFunction = GpioPin::AlternateFunction::AF5;

Spi spiMaster() {
   return Spi{ SPI2 };
}
Spi spiSlave() {
   return Spi{ SPI1 };
}

// GPDMA1 channels 0 to 2: no other code of the firmware uses GPDMA1.
Dma::Channel tmsChannel() {
   return { GPDMA1, 0 };
}
Dma::Channel tdoChannel() {
   return { GPDMA1, 1 };
}
Dma::Channel tdiChannel() {
   return { GPDMA1, 2 };
}

constexpr Dma::Config toSpi(Dma::Request request) {
   return { .direction = Dma::Direction::MemoryToPeripheral, .request = request, .sourceIncrement = true };
}

constexpr Dma::Config tmsConfig = toSpi(Dma::Request::spi1_tx);
constexpr Dma::Config tdiConfig = toSpi(Dma::Request::spi2_tx);
constexpr Dma::Config tdoConfig{ .direction = Dma::Direction::PeripheralToMemory,
                                 .request = Dma::Request::spi2_rx,
                                 .destinationIncrement = true };

constexpr Spi::BaudRatePrescaler prescalerOf(uint8_t shift) {
   return static_cast<Spi::BaudRatePrescaler>(static_cast<uint32_t>(shift - 1u) << SPI_CFG1_MBR_Pos);
}

// The 8-bit data registers: one DMA byte write puts one frame in the FIFO.
void *txdr(SPI_TypeDef *spi) {
   return const_cast<uint32_t *>(&spi->TXDR);
}
void *rxdr(SPI_TypeDef *spi) {
   return const_cast<uint32_t *>(&spi->RXDR);
}

// SPI1 slave, transmit only: clocked by TCK on the link, TMS on MISO. SSM
// with SSI = 0: the slave is always selected. On an underrun (a late TMS
// byte), the slave sets UDR. UDR shows in SR only a few SCK cycles later
// (RM0481, note to figure 826), so an underrun in the last frames is not seen.
void startSlave() {
   RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
   RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
   const Spi slave = spiSlave();
   slave.configure(Spi::BaudRatePrescaler::Div2, Spi::Mode::Slave, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                   Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft, Spi::Direction::TxOnly);
}

// The SPI clocks the pins only in their alternate function. TCK is high
// in both: the master idles SCK high, also with SPE = 0 (AFCNTR). The
// change of TDI and TMS does not clock the TAP: TCK does not change.
void pinsToSpi() {
   Tdi::setMode<GpioPin::Mode::Alternate>();
   Tms::setMode<GpioPin::Mode::Alternate>();
   Tck::setMode<GpioPin::Mode::Alternate>();
   Tdo::setMode<GpioPin::Mode::Alternate>();
}

void pinsToGpio() {
   Tck::set();
   Tck::setMode<GpioPin::Mode::Output>();
   Tdi::setMode<GpioPin::Mode::Output>();
   Tms::setMode<GpioPin::Mode::Output>();
   Tdo::setMode<GpioPin::Mode::Input>();
}

// The closing order of RM0481 58.4.14: DMA, SPI, then the DMA enables.
void stop() {
   for(const auto &channel : { tmsChannel(), tdoChannel(), tdiChannel() }) {
      channel.abort();
   }
   for(const Spi &spi : { spiSlave(), spiMaster() }) {
      spi.disable();
      spi.disableTxDma();
      spi.disableRxDma();
      spi.clearAllFlags();
   }
}

// The engine choice at the 125 MHz kernel clock (docs/usb-device.md):
// bit-bang up to 3906 kHz, then the SPI up to 7.8 MHz.
static_assert(jtag::spiShiftFor(125'000'000, 2000) == 0);
static_assert(jtag::spiShiftFor(125'000'000, 3900) == 0);
static_assert(jtag::spiShiftFor(125'000'000, 4000) == 5);
static_assert(jtag::spiShiftFor(125'000'000, 8000) == 4);
static_assert(jtag::spiShiftFor(125'000'000, 100'000) == 4);

} // namespace

void JtagSpi::init() {
   Rcc::enableSPI1Clock();
   Rcc::enableSPI2Clock();
   Rcc::enableGPDMA1Clock();
   // The AF number counts only in the alternate function mode: the pins
   // stay GPIO until a transfer. The TCK link is a slave clock input, which
   // needs the alternate function. board::swjConfig() frees PB3.
   for(const Pin pin : { current.debug.tck, current.debug.tdi, current.debug.tdo, current.debug.tmsOut, tckLink }) {
      gpioPin(pin).setAlternateFunction(spiFunction);
   }
   gpioPin(tckLink).configure(GpioPin::Mode::Alternate);
   const Spi master = spiMaster();
   clockHz_ = master.getClockFrequency();

   master.configure(prescalerOf(shift_), Spi::Mode::Master, Spi::ClockPolarity::High, Spi::ClockPhase::TwoEdge,
                    Spi::DataSize::Bits8, Spi::FirstBit::Lsb, Spi::NssMode::Soft);
   master.enableAlternateFunctionControl();
}

void JtagSpi::fini() {
   stop();
   gpioPin(tckLink).configure(GpioPin::Mode::Input);
}

void JtagSpi::setShift(uint8_t shift) {
   shift_ = shift;
   spiMaster().setBaudRatePrescaler(prescalerOf(shift));
}

bool JtagSpi::transfer(JtagPins &pins, const uint8_t *tdi, const uint8_t *tms, uint8_t *tdo, uint32_t bytes) {
   const Dma::Channel tmsDma = tmsChannel();
   const Dma::Channel tdoDma = tdoChannel();
   const Dma::Channel tdiDma = tdiChannel();
   const Spi slave = spiSlave();
   const Spi master = spiMaster();
   const auto count = static_cast<uint16_t>(bytes);
   // Twice the time of the bits, and 100 us for the setup
   const uint32_t cyclesPerBit = (pins.coreHz() / clockHz_) << shift_;
   const uint32_t timeout = bytes * 8u * cyclesPerBit * 2u + pins.coreHz() / 10'000u;

   // The opening order of RM0481 58.4.14: DMA channels, TXDMAEN, SPE.
   startSlave();
   tmsDma.start(tmsConfig, tms, txdr(SPI1), count, Dma::Priority::very_high);
   slave.enableTxDma();
   slave.enable();
   // The slave must have its first TMS bytes in the FIFO before the first
   // TCK edge: wait for a full FIFO or for the end of the DMA.
   uint32_t begin = pins.now();
   while(slave.isTxPacketSpaceAvailable() && !tmsDma.isTransferCompleteFlagSet()) {
      if(pins.now() - begin >= timeout) {
         stop();
         return false;
      }
   }

   // A late TMS byte gives a wrong TMS bit (UDR), a late TDO read loses a
   // TDO byte (OVR). A late TDI byte only stops the clock.
   master.setTransferSize(count);
   master.enableRxDma();
   tdoDma.start(tdoConfig, rxdr(SPI2), tdo, count, Dma::Priority::very_high);
   tdiDma.start(tdiConfig, tdi, txdr(SPI2), count, Dma::Priority::high);
   master.enableTxDma();
   master.enable();
   pinsToSpi();
   master.startTransfer();

   // Wait on the cycle counter (not on the bus matrix) for the time of the
   // bits: a DMA register poll takes AHB cycles from the three channels.
   begin = pins.now();
   const uint32_t bitCycles = bytes * 8u * cyclesPerBit;
   while(pins.now() - begin < bitCycles) {
   }
   bool ok = true;
   while(!tdoDma.isTransferCompleteFlagSet()) {
      if(pins.now() - begin >= timeout) {
         ok = false;
         break;
      }
   }
   ok = ok && !slave.isUnderrun() && !master.isOverrun();
   // The last TDO byte came in with the last rising edge: TCK is high. The
   // GPIO now drives the pins, so the SPI disable cannot cut a clock pulse
   // (ES0621 2.17.2).
   pinsToGpio();
   stop();
   return ok;
}

} // namespace board
