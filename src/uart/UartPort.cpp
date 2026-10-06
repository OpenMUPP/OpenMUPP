#include "uart/UartPort.h"

#include <cmsis_device.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>
#include <utils/Interrupt.h>

#include "board/Board.h"
#include "board/BoardPins.h"

namespace uart {

namespace {

using board::Pin;
using board::Port;

// USART1 to USART3 of the STM32F103 with their default pins (RM0008
// 9.3.8, no remap).
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
static_assert(uartPins.usart >= 1 && uartPins.usart <= 3, "the STM32F103 has USART1 to USART3");
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
   board::gpioPin(uartPins.tx)
   .configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::None, hw::GpioPin::Speed::High);
   board::gpioPin(uartPins.rx).configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);

   Interrupt::connectMemberHandler<UartPort, &UartPort::handleInterrupt>(peripheral.irq, *this);
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

void UartPort::handleInterrupt() {
   // RXNE, and on USART v1 also an overrun (ORE) with RXNE set. The SR read
   // here and the DR read in getData() clear both.
   if(usart_.isRxRegisterNotEmpty()) {
      buffers_.received(usart_.getData());
   }
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
