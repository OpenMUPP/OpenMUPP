// SPDX-License-Identifier: GPL-3.0-or-later

#include "uart/UartPort.h"

#include <atomic>
#include <cmsis_device.h>
#include <peripherals/GpioPin.h>
#include <utils/Interrupt.h>
#if !defined(__riscv)
#include <peripherals/Rcc.h>
#else
#include <Afio.h>
#include <peripherals/Dma.h>
#include <peripherals/Rcc.h>
// The CH32V30x status register (STATR) has the bits of the STM32F1 SR at the same positions, with other names
#define USART_SR_RXNE USART_STATR_RXNE
#define USART_SR_ORE USART_STATR_ORE
#define USART_SR_FE USART_STATR_FE
#define USART_SR_NE USART_STATR_NE
#define USART_SR_PE USART_STATR_PE
#endif

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"

namespace uart {

namespace {

using board::Pin;
using board::Port;

// USART1 to USART3 of the STM32F103 with their default pins (RM0008
// 9.3.8, no remap). The STM32H5 has the same pins for USART1 and USART2,
// on AF7 (DS14539 table 16). UART7 of the CH32V307 with its default pins
// (AFIO_PCFR2.USART7_RM = 00, the reset value; CH32FV2x_V3xRM table 10-30).
struct Peripheral {
   uintptr_t base;
   IRQn_Type irq;
   Pin tx;
   Pin rx;
};

constexpr Peripheral peripheralOf(uint8_t usart) {
   switch(usart) {
#if defined(__riscv)
   case 7:
      return { UART7_BASE, UART7_IRQn, { Port::C, 2 }, { Port::C, 3 } };
#endif
   case 2:
      return { USART2_BASE, USART2_IRQn, { Port::A, 2 }, { Port::A, 3 } };
   case 3:
      return { USART3_BASE, USART3_IRQn, { Port::B, 10 }, { Port::B, 11 } };
   default:
      return { USART1_BASE, USART1_IRQn, { Port::A, 9 }, { Port::A, 10 } };
   }
}

static_assert(board::current.uart.has_value(), "the UART bridge needs the UART of the board definition");
constexpr board::Uart uartPins = *board::current.uart;
constexpr Peripheral peripheral = peripheralOf(uartPins.usart);
#if defined(__riscv)
// UART7 is on APB1. USART1 is on APB2, and PCLK2 changes during each Vref measurement (board/BoardPower.cpp).
static_assert(uartPins.usart == 7, "the CH32V307 bridge supports UART7");
// The UART7 remap (USART7_RM = 01, RM table 10-30): TX on PA6, RX on PA7
constexpr bool uart7Remap = uartPins.tx == Pin{ Port::A, 6 } && uartPins.rx == Pin{ Port::A, 7 };
static_assert(uart7Remap || (uartPins.tx == peripheral.tx && uartPins.rx == peripheral.rx),
              "UART7 needs PC2 and PC3 (default map) or PA6 and PA7 (remap)");
#else
#if defined(STM32F1)
static_assert(uartPins.usart >= 1 && uartPins.usart <= 3, "the STM32F103 has USART1 to USART3");
#else
static_assert(uartPins.usart >= 1 && uartPins.usart <= 2, "the STM32H5 bridge supports USART1 and USART2");
#endif
static_assert(uartPins.tx == peripheral.tx && uartPins.rx == peripheral.rx, "the UART pins are not the default pins of the USART");
#endif

void enableClock() {
#if defined(__riscv)
   RCC->APB1PCENR |= RCC_UART7EN;
   if constexpr(uart7Remap) {
      hw::Afio::remapUsart7(hw::Afio::Usart7Remap::Remap1);
   }
   Rcc::enableDMA2Clock();
#else
   if constexpr(uartPins.usart == 1) {
      Rcc::enableUSART1Clock();
   } else if constexpr(uartPins.usart == 2) {
      Rcc::enableUSART2Clock();
   } else {
      Rcc::enableUSART3Clock();
   }
#endif
}

// hw::Usart has no clear function for the receive errors of USART v2
[[maybe_unused]] USART_TypeDef *registers() {
   return reinterpret_cast<USART_TypeDef *>(peripheral.base);
}

#if defined(__riscv)
// UART7_TX and UART7_RX requests (RM V2.5 table 11-4). The one-wire engine
// uses DMA2 channels 10 and 11 (board/BoardDefinition.h).
constexpr IRQn_Type txDmaIrq = DMA2_Channel8_IRQn;
constexpr IRQn_Type rxDmaIrq = DMA2_Channel9_IRQn;

Dma::Channel txDma() {
   return { DMA2, 8 };
}

Dma::Channel rxDma() {
   return { DMA2, 9 };
}

// The longest TX block. A shorter block gives its part of the tx ring back
// earlier, so the main loop can fill it while the DMA sends.
constexpr std::size_t maxTxBlock = 256;
#endif

constexpr hw::Usart::DataBits toUsart(DataBits dataBits) {
   return dataBits == DataBits::Seven ? hw::Usart::DataBits::Seven : hw::Usart::DataBits::Eight;
}

constexpr hw::Usart::Parity toUsart(Parity parity) {
   switch(parity) {
   case Parity::Even:
      return hw::Usart::Parity::Even;
   case Parity::Odd:
      return hw::Usart::Parity::Odd;
   case Parity::None:
      break;
   }
   return hw::Usart::Parity::None;
}

constexpr hw::Usart::StopBits toUsart(StopBits stopBits) {
   switch(stopBits) {
   case StopBits::OneAndHalf:
      return hw::Usart::StopBits::StopBits_1_5;
   case StopBits::Two:
      return hw::Usart::StopBits::StopBits_2;
   case StopBits::One:
      break;
   }
   return hw::Usart::StopBits::StopBits_1;
}

} // namespace

#if defined(__riscv)
UartPort *UartPort::instance_ = nullptr;

// The generic entry of Interrupt::connectMemberHandler calls the handler
// through a pointer, so it saves all caller-saved integer and FPU registers
// (about 90 instructions, issue #33). These entries inline the handler
// (flatten), so they save only the registers that the handler uses.
[[gnu::interrupt, gnu::flatten]] void UartPort::rxInterruptEntry() {
   instance_->serviceRx();
}

[[gnu::interrupt, gnu::flatten]] void UartPort::txInterruptEntry() {
   instance_->serviceTx();
}
#endif

UartPort::UartPort(Buffers &buffers)
   : usart_(reinterpret_cast<USART_TypeDef *>(peripheral.base))
   , buffers_(buffers) {
}

void UartPort::start() {
   enableClock();
   // The TX pin idles high. The pull-up keeps an open RX pin at the idle
   // level, so it receives no noise.
#if defined(STM32F1) || defined(__riscv)
   board::gpioPin(uartPins.tx)
   .configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::High);
   board::gpioPin(uartPins.rx).configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);
#else
   // The AF first, so the pin does not drive another function
   for(const Pin pin : { uartPins.tx, uartPins.rx }) {
      board::gpioPin(pin).setAlternateFunction(hw::GpioPin::AlternateFunction::AF7);
   }
   board::gpioPin(uartPins.tx)
   .configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::High);
   board::gpioPin(uartPins.rx).configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);
#endif

#if defined(__riscv)
   instance_ = this;
   auto *data = const_cast<uint16_t *>(&registers()->DATAR);
   const Dma::Channel rx = rxDma();
   rx.disable();
   rx.configure(data, rxDma_.data(), Dma::DataSize::Size8Bits, Dma::Direction::PeripheralToMemory, true, false);
   rx.setCircular(true);
   rx.setTransferCount(rxDmaSize);
   rx.setPriority(Dma::ChannelPriority::very_high);
   rx.clearGlobalInterruptFlag();
   rx.enableHalfTransferInterrupt();
   rx.enableTransferCompleteInterrupt();
   // The channel waits for the requests: setFormat() turns them on.
   rx.enable();
   const Dma::Channel tx = txDma();
   tx.disable();
   tx.configure(data, nullptr, Dma::DataSize::Size8Bits, Dma::Direction::MemoryToPeripheral, true, false);
   tx.setPriority(Dma::ChannelPriority::high);
   tx.clearGlobalInterruptFlag();
   tx.enableTransferCompleteInterrupt();
   Interrupt::connectHandler(rxDmaIrq, &UartPort::rxInterruptEntry);
   Interrupt::connectHandler(txDmaIrq, &UartPort::txInterruptEntry);
   for(const IRQn_Type irq : { rxDmaIrq, txDmaIrq }) {
      NVIC_SetPriority(irq, hardware::uartIrqPriority);
      NVIC_ClearPendingIRQ(irq);
      NVIC_EnableIRQ(irq);
   }
#else
   Interrupt::connectMemberHandler<UartPort, &UartPort::handleInterrupt>(peripheral.irq, *this);
   NVIC_SetPriority(peripheral.irq, hardware::uartIrqPriority);
   NVIC_ClearPendingIRQ(peripheral.irq);
   NVIC_EnableIRQ(peripheral.irq);
#endif
}

uint32_t UartPort::clockFrequency() const {
   return usart_.getClockFrequency();
}

void UartPort::setFormat(const FrameFormat &format) {
   // Fails only for 7 data bits without parity, which toFrameFormat()
   // does not give.
   static_cast<void>(usart_.setFrameFormat(toUsart(format.dataBits), toUsart(format.parity), toUsart(format.stopBits)));
   usart_.setBaudRate(format.baudRate);
   // The first call enables the USART: with the format set, the TX pin
   // sends no frame with a wrong baud rate.
   usart_.enableTx();
   usart_.enableRx();
#if defined(__riscv)
   dataMask_ = format.dataBits == DataBits::Seven ? 0x7F : 0xFF;
   usart_.enableDmaRx();
   usart_.enableDmaTx();
#else
   usart_.enableRxNotEmptyInterrupt();
#endif
   usart_.enable();
}

#if defined(__riscv)
bool UartPort::isTxIdle() const {
   return txLength_ == 0 && usart_.isTransferComplete();
}

void UartPort::startTx() {
   txEnabled_ = true;
   // The interrupt starts a block when the DMA is idle.
   NVIC_SetPendingIRQ(txDmaIrq);
}

void UartPort::stopTx() {
   Interrupt::CriticalSection cs;
   txEnabled_ = false;
   const Dma::Channel dma = txDma();
   dma.disable();
   dma.clearGlobalInterruptFlag();
   if(txLength_ != 0) {
      // The bytes that the DMA has written to the UART leave tx.
      buffers_.tx.commit_read(txLength_ - dma.getDataCount());
      txLength_ = 0;
   }
}

void UartPort::pollRx() {
   // The interrupt of the channel also runs serviceRx(): only one at a time.
   Interrupt::CriticalSection cs;
   serviceRx();
}

namespace {

// A lap of a circular DMA buffer of size bytes: the DMA passed the half and
// the end since the copy that ended at tail (both flags valid), and it is now
// at head. Without a lap the DMA moved at least up to the last flag position
// on the way. Less means that it wrote size bytes or more. A lap of more
// than size + the distance to the last flag gives no count.
constexpr bool isLap(std::size_t tail, std::size_t head, std::size_t size) {
   const std::size_t moved = (head + size - tail) % size;
   const std::size_t lastFlag = tail < size / 2 ? size : size + size / 2;
   return moved < lastFlag - tail;
}

static_assert(!isLap(250, 128, 256)); // 134 bytes: TC and HT, no lap
static_assert(isLap(250, 38, 256));   // 300 bytes
static_assert(isLap(250, 250, 256));  // 256 bytes
static_assert(isLap(250, 122, 256));  // 384 bytes
static_assert(!isLap(10, 0, 256));    // 246 bytes: HT and TC, no lap
static_assert(isLap(10, 255, 256));   // 501 bytes
static_assert(!isLap(10, 2, 256));    // 504 bytes: not seen

} // namespace

void UartPort::serviceRx() {
   const Dma::Channel dma = rxDma();
   // The DMA passed the half (HT) and the end (TC) of rxDma_ since the last
   // pass. A flag can be from a pass that ended exactly at its position: the
   // DMA set it between the clear and the CNTR read. Such a flag is ignored,
   // so a lap that starts at a flag position is not counted.
   const std::size_t tail = rxTail_;
   const bool passedBoth = (dma.isHalfTransferInterruptFlagSet() && tail != rxDmaSize / 2) &&
                           (dma.isTransferCompleteInterruptFlagSet() && tail != 0);
   dma.clearGlobalInterruptFlag();
   const uint32_t status = usart_.returnStatusFlag();
   // CNTR counts down from rxDmaSize and reloads after 0 (circular).
   const std::size_t head = (rxDmaSize - dma.getDataCount()) % rxDmaSize;
   // The DMA wrote the bytes up to head before the CNTR read.
   std::atomic_signal_fence(std::memory_order_acquire);
   // A lap: the DMA wrote rxDmaSize bytes or more since the last pass, so it
   // wrote over bytes that are not copied. Count one overrun and drop the
   // buffer: its oldest bytes are overwritten.
   if(passedBoth && isLap(tail, head, rxDmaSize)) {
      buffers_.overrun();
      rxTail_ = head;
      return;
   }
   // An error flag clears only when a data register read follows a status
   // read, and the DMA does the read with the next byte. Thus a flag stays
   // set between two checks when no byte came: count it again only when
   // the DMA has moved. Two errors between two checks give one count.
   // The status read of isTxIdle() can also clear a flag before this check.
   if((status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0 && head != errorHead_) {
      errorHead_ = head;
      if((status & USART_SR_ORE) != 0) {
         buffers_.overrun();
      }
      if((status & (USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0) {
         buffers_.receiveError((status & USART_SR_PE) != 0);
      }
   }
   std::size_t next = tail;
   while(next != head) {
      buffers_.received(rxDma_[next] & dataMask_);
      next = (next + 1) % rxDmaSize;
   }
   rxTail_ = next;
}

void UartPort::serviceTx() {
   const Dma::Channel dma = txDma();
   if(txLength_ != 0) {
      // startTx() requests the interrupt also while a block is sent.
      if(!dma.isTransferCompleteInterruptFlagSet()) {
         return;
      }
      dma.clearGlobalInterruptFlag();
      dma.disable();
      buffers_.tx.commit_read(txLength_);
      txLength_ = 0;
   }
   if(!txEnabled_) {
      return;
   }
   std::size_t length = 0;
   const uint8_t *data = buffers_.tx.read_ptr(length);
   if(length == 0) {
      return;
   }
   length = length < maxTxBlock ? length : maxTxBlock;
   // TC is set from the last block. isTxIdle() reads it after this block.
   usart_.clearStatusFlag(USART_STATR_TC);
   dma.setMemoryAddress(reinterpret_cast<uintptr_t>(data));
   dma.setTransferCount(static_cast<uint16_t>(length));
   txLength_ = length;
   dma.enable();
}
#else
bool UartPort::isTxIdle() const {
   return !usart_.isTxEmptyInterruptEnabled() && usart_.isTransferComplete();
}

void UartPort::startTx() {
   // CR1 is a read-modify-write; the interrupt clears TXEIE in CR1.
   Interrupt::CriticalSection cs;
   usart_.enableTxEmptyInterrupt();
}

void UartPort::stopTx() {
   Interrupt::CriticalSection cs;
   usart_.disableTxEmptyInterrupt();
}

void UartPort::pollRx() {
}

void UartPort::handleInterrupt() {
   const uint32_t status = usart_.returnStatusFlag();
#if defined(USART_SR_RXNE)
   // One SR read for all receive flags. On USART v1 the receive interrupt
   // fires for RXNE and for an overrun (ORE). ORE can come without RXNE:
   // the next byte completes between the SR read and the DR read of an
   // earlier pass. Only the SR read followed by a DR read clears ORE (RM0008
   // 27.6.1), so the DR read is done for ORE too. Otherwise the interrupt
   // fires again and again until the next byte arrives.
   if((status & (USART_SR_RXNE | USART_SR_ORE)) != 0) {
      const uint8_t byte = usart_.getData();
      if((status & USART_SR_ORE) != 0) {
         buffers_.overrun();
      }
      if((status & USART_SR_RXNE) != 0) {
         if((status & (USART_SR_FE | USART_SR_NE | USART_SR_PE)) != 0) {
            buffers_.receiveError((status & USART_SR_PE) != 0);
         }
         buffers_.received(byte);
      }
   }
#else
   // USART v2 (STM32H5, FIFO off): the error flags stay set until a write
   // to ICR clears them (RM0481 50.8.10). The receive interrupt fires for
   // ORE too, so ORE is cleared here, else the interrupt fires again.
   if((status & USART_ISR_ORE) != 0) {
      registers()->ICR = USART_ICR_ORECF;
      buffers_.overrun();
   }
   if((status & USART_ISR_RXNE_RXFNE) != 0) {
      if((status & (USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0) {
         registers()->ICR = USART_ICR_FECF | USART_ICR_NECF | USART_ICR_PECF;
         buffers_.receiveError((status & USART_ISR_PE) != 0);
      }
      buffers_.received(usart_.getData());
   }
#endif
   if(usart_.isTxEmptyInterruptEnabled() && usart_.isTxRegisterEmpty()) {
      uint8_t byte = 0;
      if(buffers_.nextToSend(byte)) {
         usart_.setData(byte);
      } else {
         usart_.disableTxEmptyInterrupt();
      }
   }
}
#endif

} // namespace uart
