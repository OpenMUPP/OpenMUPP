// SPDX-License-Identifier: GPL-3.0-or-later

#include "swo/SwoReceiver.h"

#include <algorithm>
#include <atomic>
#include <peripherals/Dma.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>
#include <peripherals/Timer.h>
#include <peripherals/Usart.h>
#include <utility>
#include <utils/Interrupt.h>

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"

// CH32FV2x_V3xRM chapters 15 (general-purpose timers), 21 (USART), and
// tables 11-2 and 11-3 (DMA requests).

namespace swo {

namespace {

using cmsisdap::swoMode::manchester;
using cmsisdap::swoMode::uart;

static_assert(board::current.swo.has_value(), "the SWO receiver needs the SWO pin of the board definition");
// board/BoardChecks.cpp checks that the pin is the RX pin of the USART.
static_assert(board::current.swo->usart == 2, "the CH32V307 SWO receiver supports USART2 (RX on PA3)");
// TIM5_CH4 is on PA3 in the default map (RM table 10-19).
static_assert(board::current.swo->rx == board::Pin{ board::Port::A, 3 }, "the Manchester capture needs TIM5_CH4 on PA3");

// USART2_RX request. The JTAG fast path uses DMA1 channels 4 and 5, the
// UART bridge DMA2 channels 8 and 9 (board/BoardDefinition.h).
constexpr IRQn_Type dmaIrq = DMA1_Channel6_IRQn;

Dma::Channel dma() {
   return { DMA1, 6 };
}

hw::Usart usart() {
   return hw::Usart{ USART2 };
}

// TIM5_CH4 (rising edges) and TIM5_CH3 (falling edges). Channel 2 is also
// the TMS channel of the JTAG fast path (swo/SwoReceiver.h). The one-wire
// engine uses DMA2 channels 10 and 11.
constexpr IRQn_Type risingIrq = DMA2_Channel1_IRQn;
constexpr IRQn_Type fallingIrq = DMA2_Channel2_IRQn;

Dma::Channel risingDma() {
   return { DMA2, 1 };
}
Dma::Channel fallingDma() {
   return { DMA2, 2 };
}

hw::Timer timer() {
   return hw::Timer{ TIM5 };
}

// The bits of channels 3 and 4 that the family header does not define
// (RM 15.4.4, 15.4.8, 15.4.9).
constexpr uint32_t cc3sFromTi4 = 2U << 0; // CHCTLR2: IC3 on TI4
constexpr uint32_t cc4sFromTi4 = 1U << 8; // CHCTLR2: IC4 on TI4
// The input filter: 8 samples at the timer clock (56 ns at 144 MHz). It
// delays both edges by the same time.
constexpr uint32_t inputFilter = 3U;
constexpr uint32_t ic3f = inputFilter << 4;
constexpr uint32_t ic4f = inputFilter << 12;
constexpr uint32_t cc3e = 1U << 8;
constexpr uint32_t cc3p = 1U << 9; // falling edge
constexpr uint32_t cc4e = 1U << 12;
constexpr uint32_t cc3de = 1U << 11;
constexpr uint32_t cc4de = 1U << 12;
// The reset bit of TIM5 in APB1PRSTR has the position of its clock enable bit.
constexpr uint32_t tim5Reset = RCC_TIM5EN;

constexpr board::Pin rxPin = board::current.swo->rx;

// A request above the maximum (divider 16) gives the maximum, as in the
// reference implementation (SWO_Baudrate_UART()).
uint32_t limited(uint32_t clock, uint32_t baud) {
   return std::min(baud, clock / 16);
}

// The timer clock of APB1: PCLK1, or twice PCLK1 when APB1 is divided
// (RM figure 3-3).
uint32_t timerClock() {
   const uint32_t pclk1 = Rcc::getAPBClock();
   constexpr uint32_t divided = 4U << 8; // the high bit of PPRE1
   return (RCC->CFGR0 & divided) != 0 ? pclk1 * 2 : pclk1;
}

std::optional<ManchesterTiming> manchesterTimingFor(uint32_t baud) {
   return manchesterTiming(timerClock(), std::min(baud, SwoReceiver::maxManchesterBaud));
}

// Counts the HT and TC flags of a circular channel. Clears only the flag
// that is counted: a clear of both flags could drop a flag that comes
// between the check and the clear. Two flags: the interrupt is one half
// late, and both halves count.
uint32_t countHalves(const Dma::Channel &channel) {
   uint32_t done = 0;
   if(channel.isHalfTransferInterruptFlagSet()) {
      channel.clearHalfTransferInterruptFlag();
      ++done;
   }
   if(channel.isTransferCompleteInterruptFlagSet()) {
      channel.clearTransferCompleteInterruptFlag();
      ++done;
   }
   return done;
}

void startCircular(const Dma::Channel &channel,
                   const volatile void *peripheral,
                   void *memory,
                   Dma::DataSize size,
                   std::size_t count,
                   IRQn_Type irq,
                   void (*handler)()) {
   channel.disable();
   channel.configure(const_cast<void *>(peripheral), memory, size, Dma::Direction::PeripheralToMemory, true, false);
   channel.setCircular(true);
   channel.setTransferCount(static_cast<uint16_t>(count));
   channel.setPriority(Dma::ChannelPriority::high);
   channel.clearGlobalInterruptFlag();
   channel.enableHalfTransferInterrupt();
   channel.enableTransferCompleteInterrupt();
   Interrupt::connectHandler(irq, handler);
   NVIC_SetPriority(irq, hardware::uartIrqPriority);
   NVIC_ClearPendingIRQ(irq);
   NVIC_EnableIRQ(irq);
   channel.enable();
}

// Leaves the channel as after reset for the next user: the JTAG fast path
// sets only the addresses, the sizes and the direction.
void stopCircular(const Dma::Channel &channel, IRQn_Type irq) {
   NVIC_DisableIRQ(irq);
   channel.disable();
   channel.setCircular(false);
   channel.disableAllInterrupts();
   channel.clearGlobalInterruptFlag();
}

} // namespace

SwoReceiver *SwoReceiver::instance_ = nullptr;

namespace {
SwoRing<SwoReceiver::bufferSize> traceRing;
ManchesterEdges<SwoReceiver::edgeBufferSize> edgeRings;
} // namespace

SwoRing<SwoReceiver::bufferSize> &SwoReceiver::ring() {
   return traceRing;
}

ManchesterEdges<SwoReceiver::edgeBufferSize> &SwoReceiver::edges() {
   return edgeRings;
}

// Direct entries, as in uart/UartPort.cpp: they save only the registers
// that the handler uses.
[[gnu::interrupt, gnu::flatten]] void SwoReceiver::dmaInterruptEntry() {
   instance_->halves_ = instance_->halves_ + countHalves(dma());
}

[[gnu::interrupt, gnu::flatten]] void SwoReceiver::risingInterruptEntry() {
   instance_->risingHalves_ = instance_->risingHalves_ + countHalves(risingDma());
}

[[gnu::interrupt, gnu::flatten]] void SwoReceiver::fallingInterruptEntry() {
   instance_->fallingHalves_ = instance_->fallingHalves_ + countHalves(fallingDma());
}

uint32_t SwoReceiver::realBaud(uint8_t mode, uint32_t baud) const {
   if(mode == manchester) {
      return manchesterTimingFor(baud) ? std::min(baud, maxManchesterBaud) : 0;
   }
   if(mode != uart) {
      return 0;
   }
   const uint32_t clock = usart().getClockFrequency();
   return swo::realBaud(clock, limited(clock, baud));
}

uint32_t SwoReceiver::start(uint8_t mode, uint32_t baud) {
   stop();
   if(mode == manchester) {
      return startManchester(baud);
   }
   if(mode == uart) {
      return startUart(baud);
   }
   return 0;
}

uint32_t SwoReceiver::startUart(uint32_t baud) {
   Rcc::enableUSART2Clock();
   Rcc::enableDMA1Clock();
   const hw::Usart port = usart();
   baud = limited(port.getClockFrequency(), baud);
   const auto divider = usartDivider(port.getClockFrequency(), baud);
   if(!divider) {
      return 0;
   }
   instance_ = this;
   mode_ = uart;
   ring().reset();
   halves_ = 0;
   overrun_ = false;
   framingError_ = false;

   // The pull-up keeps an open line at the idle level (high), so it gives
   // no framing errors. The target side of openmupp-v1 has its own pull-up.
   board::gpioPin(rxPin).configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);

   startCircular(dma(), &port.usart.DATAR, ring().data(), Dma::DataSize::Size8Bits, bufferSize, dmaIrq, &SwoReceiver::dmaInterruptEntry);

   // 8N1, receiver only (TE = 0: PA2 stays a GPIO), DMA requests on
   port.usart.CTLR1 = 0;
   port.usart.CTLR2 = 0;
   port.usart.CTLR3 = 0;
   port.setBRR(*divider);
   port.enableDmaRx();
   port.enableRx();
   // An empty data register: reading STATR and then DATAR clears RXNE and
   // the error flags.
   static_cast<void>(port.getStatusFlags());
   static_cast<void>(port.getData());
   port.enable();
   active_ = true;
   return swo::realBaud(port.getClockFrequency(), baud);
}

uint32_t SwoReceiver::startManchester(uint32_t baud) {
   const auto timing = manchesterTimingFor(baud);
   if(!timing) {
      return 0;
   }
   Rcc::detail::enableClock(RCC->APB1PCENR, RCC_TIM5EN);
   Rcc::detail::resetPeripheral(RCC->APB1PRSTR, tim5Reset);
   Rcc::enableDMA2Clock();
   instance_ = this;
   mode_ = manchester;
   ring().reset();
   decoded_ = 0;
   risingHalves_ = 0;
   fallingHalves_ = 0;
   overrun_ = false;
   framingError_ = false;

   // The pull-down keeps an open line at the idle level (low). The target
   // side of openmupp-v1 has a 10 kOhm pull-up: without a target, the line
   // is high, and the decoder waits.
   const hw::GpioPin pin = board::gpioPin(rxPin);
   pin.configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Down);

   const hw::Timer t = timer();
   t.setPrescaler(timing->prescaler);
   t.setAutoReload(0xFFFF);
   // Loads the prescaler. The update flag is not used.
   t.generateUpdate();
   t.clearAllStatusFlags();
   t.tim.CHCTLR2 = cc3sFromTi4 | ic3f | cc4sFromTi4 | ic4f;
   t.tim.CCER = cc3e | cc3p | cc4e;

   startCircular(risingDma(), &t.tim.CH4CVR, edges().rising(), Dma::DataSize::Size16Bits, edgeBufferSize, risingIrq,
                 &SwoReceiver::risingInterruptEntry);
   startCircular(fallingDma(), &t.tim.CH3CVR, edges().falling(), Dma::DataSize::Size16Bits, edgeBufferSize, fallingIrq,
                 &SwoReceiver::fallingInterruptEntry);
   t.tim.DMAINTENR = cc3de | cc4de;

   const bool high = pin.read();
   edges().reset(high);
   checkedRising_ = 0;
   checkedFalling_ = 0;
   levelMismatch_ = false;
   decoder_.configure(*timing);
   decoder_.reset(high, 0);
   t.enableCounter();
   active_ = true;
   return std::min(baud, maxManchesterBaud);
}

uint32_t SwoReceiver::setBaud(uint32_t baud) {
   if(!active_) {
      return 0;
   }
   if(mode_ == manchester) {
      // The decoder starts again with the new timing. The ring stays.
      const auto timing = manchesterTimingFor(baud);
      if(!timing) {
         return 0;
      }
      poll();
      const hw::Timer t = timer();
      t.setPrescaler(timing->prescaler);
      t.generateUpdate();
      decoder_.configure(*timing);
      decoder_.reset(edges().lineHigh(), static_cast<uint16_t>(t.getCounter()));
      return std::min(baud, maxManchesterBaud);
   }
   const hw::Usart port = usart();
   baud = limited(port.getClockFrequency(), baud);
   const auto divider = usartDivider(port.getClockFrequency(), baud);
   if(!divider) {
      return 0;
   }
   // The DMA keeps its position. A byte on the line during the change can
   // give a framing error.
   port.disable();
   port.setBRR(*divider);
   port.enable();
   return swo::realBaud(port.getClockFrequency(), baud);
}

void SwoReceiver::stop() {
   if((RCC->APB1PCENR & RCC_USART2EN) != 0) {
      usart().disable();
   }
   if(!active_) {
      return;
   }
   if(mode_ == manchester) {
      // The bytes of the edges in the rings
      poll();
      stopManchester();
   } else {
      NVIC_DisableIRQ(dmaIrq);
      dma().disable();
   }
   // The line floats again: on the YD-CH32V307VCT6, PA3 is wired to TDO.
   board::gpioPin(rxPin).configure(hw::GpioPin::Mode::Input);
   active_ = false;
}

void SwoReceiver::stopManchester() {
   // The reset stops the DMA requests of TIM5 on DMA2 channel 2.
   Rcc::detail::resetPeripheral(RCC->APB1PRSTR, tim5Reset);
   RCC->APB1PCENR &= ~RCC_TIM5EN;
   stopCircular(risingDma(), risingIrq);
   stopCircular(fallingDma(), fallingIrq);
}

uint32_t SwoReceiver::written() const {
   if(mode_ == manchester) {
      return decoded_;
   }
   uint32_t count = 0;
   {
      // halves_ and the counter from the same moment (see SwoRing::writeCount())
      Interrupt::CriticalSection cs;
      count = SwoRing<bufferSize>::writeCount(halves_, bufferSize - dma().getDataCount());
   }
   // The DMA wrote the bytes up to count before the counter read.
   std::atomic_signal_fence(std::memory_order_acquire);
   return count;
}

void SwoReceiver::poll() {
   if(!active_ || mode_ != manchester) {
      return;
   }
   using Ring = SwoRing<edgeBufferSize>;
   const auto counts = [this](uint16_t *now) {
      Interrupt::CriticalSection cs;
      // The time before the DMA counters: an edge after it is not in the
      // counts, so idle() does not end a frame that goes on.
      if(now != nullptr) {
         *now = static_cast<uint16_t>(timer().getCounter());
      }
      const uint32_t rising = Ring::writeCount(risingHalves_, edgeBufferSize - risingDma().getDataCount());
      const uint32_t falling = Ring::writeCount(fallingHalves_, edgeBufferSize - fallingDma().getDataCount());
      return std::pair{ rising, falling };
   };
   uint16_t now = 0;
   const auto [rising, falling] = counts(&now);
   std::atomic_signal_fence(std::memory_order_acquire);
   const auto sink = [this](uint8_t byte) {
      ring().data()[decoded_ % bufferSize] = byte;
      ++decoded_;
   };
   bool lost = !edges().merge(rising, falling, decoder_, sink);
   if(!lost) {
      const auto [risingAfter, fallingAfter] = counts(nullptr);
      lost = edges().lapped(risingAfter, fallingAfter);
      if(lost) {
         edges().drop(risingAfter, fallingAfter);
      }
   }
   if(lost) {
      overrun_ = true;
      decoder_.reset(edges().lineHigh(), static_cast<uint16_t>(timer().getCounter()));
      return;
   }
   decoder_.idle(now);
   checkLevel(rising, falling, now);
   framingError_ = framingError_ || decoder_.takeError();
}

// An edge while the capture starts can give the edge streams the wrong
// order. Then the line level of the counts is not the level of the pin, or
// an edge waits in one stream for an edge of the other stream. Both can
// also come from an edge that is not in the counts yet. Two such polls
// with no new edge between them correct the order.
void SwoReceiver::checkLevel(uint32_t rising, uint32_t falling, uint16_t now) {
   const bool high = board::gpioPin(rxPin).read();
   const bool quiet = rising == checkedRising_ && falling == checkedFalling_;
   checkedRising_ = rising;
   checkedFalling_ = falling;
   const bool wrong = high != edges().lineHigh() || edges().waiting(rising, falling);
   if(!wrong || !quiet) {
      levelMismatch_ = false;
      return;
   }
   if(!levelMismatch_) {
      levelMismatch_ = true;
      return;
   }
   levelMismatch_ = false;
   edges().realign(rising, falling, high);
   decoder_.reset(high, now);
   framingError_ = true;
}

void SwoReceiver::sampleErrors() {
   if(!active_ || mode_ != uart) {
      return;
   }
   // The DMA reads DATAR, so a flag clears with the next byte after this read.
   const uint32_t flags = usart().getStatusFlags();
   overrun_ = overrun_ || (flags & USART_STATR_ORE) != 0;
   framingError_ = framingError_ || (flags & (USART_STATR_FE | USART_STATR_NE)) != 0;
}

std::size_t SwoReceiver::read(art::span<uint8_t> data) {
   poll();
   sampleErrors();
   bool lost = false;
   const std::size_t count = ring().read([this] { return written(); }, data.data(), data.size(), lost);
   overrun_ = overrun_ || lost;
   return count;
}

SwoReceiver::Status SwoReceiver::status() {
   poll();
   sampleErrors();
   const uint32_t pending = ring().available(written());
   overrun_ = overrun_ || pending > bufferSize;
   return { active_, overrun_, framingError_, pending > bufferSize ? 0 : pending };
}

} // namespace swo
