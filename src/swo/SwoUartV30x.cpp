// SPDX-License-Identifier: GPL-3.0-or-later

#include "swo/SwoUart.h"

#include <atomic>
#include <peripherals/Dma.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>
#include <peripherals/Usart.h>
#include <utils/Interrupt.h>

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"

// CH32FV2x_V3xRM chapter 21 (USART) and table 11-3 (DMA1 requests).

namespace swo {

namespace {

static_assert(board::current.swo.has_value(), "the SWO receiver needs the SWO pin of the board definition");
// board/BoardChecks.cpp checks that the pin is the RX pin of the USART.
static_assert(board::current.swo->usart == 2, "the CH32V307 SWO receiver supports USART2 (RX on PA3)");

// USART2_RX request. The JTAG fast path uses DMA1 channels 4 and 5, the
// UART bridge DMA2 channels 8 and 9 (board/BoardDefinition.h).
constexpr IRQn_Type dmaIrq = DMA1_Channel6_IRQn;

Dma::Channel dma() {
   return { DMA1, 6 };
}

hw::Usart usart() {
   return hw::Usart{ USART2 };
}

constexpr board::Pin rxPin = board::current.swo->rx;

} // namespace

SwoUart *SwoUart::instance_ = nullptr;

// A direct entry, as in uart/UartPort.cpp: it saves only the registers that
// the handler uses.
[[gnu::interrupt, gnu::flatten]] void SwoUart::dmaInterruptEntry() {
   instance_->serviceDma();
}

void SwoUart::serviceDma() {
   const Dma::Channel channel = dma();
   // Clear only the flag that is counted: a clear of both flags could drop
   // a flag that comes between the check and the clear. Two flags: the
   // interrupt is one half late, and both halves count.
   uint32_t done = 0;
   if(channel.isHalfTransferInterruptFlagSet()) {
      channel.clearHalfTransferInterruptFlag();
      ++done;
   }
   if(channel.isTransferCompleteInterruptFlagSet()) {
      channel.clearTransferCompleteInterruptFlag();
      ++done;
   }
   halves_ = halves_ + done;
}

uint32_t SwoUart::realBaud(uint32_t baud) const {
   return swo::realBaud(usart().getClockFrequency(), baud);
}

uint32_t SwoUart::start(uint32_t baud) {
   stop();
   Rcc::enableUSART2Clock();
   Rcc::enableDMA1Clock();
   const hw::Usart port = usart();
   const auto divider = usartDivider(port.getClockFrequency(), baud);
   if(!divider) {
      return 0;
   }
   instance_ = this;
   ring_.reset();
   halves_ = 0;
   overrun_ = false;
   framingError_ = false;

   // The pull-up keeps an open line at the idle level (high), so it gives
   // no framing errors. The target side of openmupp-v1 has its own pull-up.
   board::gpioPin(rxPin).configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);

   const Dma::Channel channel = dma();
   channel.configure(const_cast<uint16_t *>(&port.usart.DATAR), ring_.data(), Dma::DataSize::Size8Bits,
                     Dma::Direction::PeripheralToMemory, true, false);
   channel.setCircular(true);
   channel.setTransferCount(static_cast<uint16_t>(bufferSize));
   channel.setPriority(Dma::ChannelPriority::high);
   channel.clearGlobalInterruptFlag();
   channel.enableHalfTransferInterrupt();
   channel.enableTransferCompleteInterrupt();
   Interrupt::connectHandler(dmaIrq, &SwoUart::dmaInterruptEntry);
   NVIC_SetPriority(dmaIrq, hardware::uartIrqPriority);
   NVIC_ClearPendingIRQ(dmaIrq);
   NVIC_EnableIRQ(dmaIrq);
   channel.enable();

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

void SwoUart::stop() {
   if((RCC->APB1PCENR & RCC_USART2EN) != 0) {
      usart().disable();
   }
   if(active_) {
      NVIC_DisableIRQ(dmaIrq);
      dma().disable();
      // The line floats again: on the YD-CH32V307VCT6, PA3 is wired to TDO.
      board::gpioPin(rxPin).configure(hw::GpioPin::Mode::Input);
      active_ = false;
   }
}

uint32_t SwoUart::written() const {
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

void SwoUart::sampleErrors() {
   if(!active_) {
      return;
   }
   // The DMA reads DATAR, so a flag clears with the next byte after this read.
   const uint32_t flags = usart().getStatusFlags();
   overrun_ = overrun_ || (flags & USART_STATR_ORE) != 0;
   framingError_ = framingError_ || (flags & (USART_STATR_FE | USART_STATR_NE)) != 0;
}

std::size_t SwoUart::read(art::span<uint8_t> data) {
   sampleErrors();
   bool lost = false;
   const std::size_t count = ring_.read([this] { return written(); }, data.data(), data.size(), lost);
   overrun_ = overrun_ || lost;
   return count;
}

SwoUart::Status SwoUart::status() {
   sampleErrors();
   const uint32_t pending = ring_.available(written());
   const Status result{ active_, overrun_ || pending > bufferSize, framingError_, pending > bufferSize ? 0 : pending };
   // An overflow stays in the ring until the next read(), so the next
   // status() reports it again.
   overrun_ = false;
   framingError_ = false;
   return result;
}

} // namespace swo
