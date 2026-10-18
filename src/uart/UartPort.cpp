#include "uart/UartPort.h"

#include <cmsis_device.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>
#include <utils/Interrupt.h>

#include "Hardware.h"
#include "board/Board.h"
#include "board/BoardPins.h"

namespace uart {

namespace {

using board::Pin;
using board::Port;

// USART1 to USART3 of the STM32F103 with their default pins (RM0008
// 9.3.8, no remap). The STM32H5 has the same pins for USART1 and USART2,
// on AF7 (DS14539 table 16).
struct Peripheral {
   uintptr_t base;
   IRQn_Type irq;
   Pin tx;
   Pin rx;
};

constexpr Peripheral peripheralOf(uint8_t usart) {
   switch(usart) {
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
#if defined(STM32F1)
static_assert(uartPins.usart >= 1 && uartPins.usart <= 3, "the STM32F103 has USART1 to USART3");
#else
static_assert(uartPins.usart >= 1 && uartPins.usart <= 2, "the STM32H5 bridge supports USART1 and USART2");
#endif
constexpr Peripheral peripheral = peripheralOf(uartPins.usart);
static_assert(uartPins.tx == peripheral.tx && uartPins.rx == peripheral.rx, "the UART pins are not the default pins of the USART");

void enableClock() {
   if constexpr(uartPins.usart == 1) {
      Rcc::enableUSART1Clock();
   } else if constexpr(uartPins.usart == 2) {
      Rcc::enableUSART2Clock();
   } else {
      Rcc::enableUSART3Clock();
   }
}

// hw::Usart has no clear function for the receive errors of USART v2
[[maybe_unused]] USART_TypeDef *registers() {
   return reinterpret_cast<USART_TypeDef *>(peripheral.base);
}

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

UartPort::UartPort(Buffers &buffers)
   : usart_(reinterpret_cast<USART_TypeDef *>(peripheral.base))
   , buffers_(buffers) {
}

void UartPort::start() {
   enableClock();
   // The TX pin idles high. The pull-up keeps an open RX pin at the idle
   // level, so it receives no noise.
#if defined(STM32F1)
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

   Interrupt::connectMemberHandler<UartPort, &UartPort::handleInterrupt>(peripheral.irq, *this);
   NVIC_SetPriority(peripheral.irq, hardware::uartIrqPriority);
   NVIC_ClearPendingIRQ(peripheral.irq);
   NVIC_EnableIRQ(peripheral.irq);
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
   usart_.enableRxNotEmptyInterrupt();
   usart_.enable();
}

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

} // namespace uart
