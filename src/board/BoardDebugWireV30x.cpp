// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardDebugWire.h"

#include <peripherals/Rcc.h>
#include <peripherals/Usart.h>

// The pins and the USART3 receiver of the debugWIRE engine on the CH32V307
// (board/BoardDebugWire.h). CH32FV2x_V3xRM chapter 21 (USART).

template class avr::DebugWireEngine<board::DebugWirePins>;

namespace board {

namespace {

using hw::GpioPin;
using hw::Usart;

// USART3 RX without a remap
static_assert(*current.debug.srstSense == Pin{ Port::B, 11 }, "the debugWIRE receiver needs the read-back pin on PB11 (USART3 RX)");

// As the AVR ISP RESET (BoardIspV30x.cpp): active low is open-drain, so the
// pull-up of the target releases the line. Active high drives a transistor.
constexpr GpioPin::OutputType driveType =
current.debug.srstActive == Active::Low ? GpioPin::OutputType::OpenDrain : GpioPin::OutputType::PushPull;

Usart usart() {
   return Usart{ USART3 };
}

} // namespace

void DebugWirePins::init() {
   rxDisable();
   configureOutput(resetSignal, false, driveType);
   // The pull-up keeps the input high without a target. It is weak (about
   // 40 kOhm) next to the pull-up of the target.
   gpioPin(*current.debug.srstSense).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
}

void DebugWirePins::fini() {
   rxDisable();
   configureOutput(resetSignal, false, driveType);
   if(current.debug.srstActive == Active::Low) {
      gpioPin(resetSignal.pin).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   gpioPin(*current.debug.srstSense).configure(GpioPin::Mode::Input);
}

bool DebugWirePins::rxEnable(uint32_t baud) {
   Rcc::enableUSART3Clock();
   const Usart port = usart();
   port.disable();
   // BRR is the bus clock / baud (16 times oversampling). The register has
   // 16 bits, and the USART needs 16 or more. At a 144 MHz bus clock (APB1
   // is not divided): 2198 baud to 9 Mbaud. Usart::setBaudRate() does not check the range.
   const uint32_t divider = baud == 0 ? 0 : Usart::calculateBRR(baud, port.getClockFrequency());
   if(divider < 16 || divider > 0xFFFF) {
      return false;
   }
   // 8N1, receiver only
   port.usart.CTLR1 = 0;
   port.usart.CTLR2 = 0;
   port.usart.CTLR3 = 0;
   port.setBRR(divider);
   port.enableRx();
   port.enable();
   // An empty buffer: reading STATR and then DATAR clears RXNE and the
   // error flags.
   static_cast<void>(port.getStatusFlags());
   static_cast<void>(port.getData());
   return true;
}

void DebugWirePins::rxDisable() {
   if((RCC->APB1PCENR & RCC_USART3EN) != 0) {
      usart().disable();
   }
}

int32_t DebugWirePins::rxRead() {
   const Usart port = usart();
   const uint32_t status = port.getStatusFlags();
   if((status & USART_STATR_RXNE) == 0) {
      return -1;
   }
   // The read of DATAR after STATR clears FE, NE and ORE too.
   const int32_t value = port.getData();
   return (status & USART_STATR_FE) != 0 ? value | avr::dwFramingError : value;
}

} // namespace board
