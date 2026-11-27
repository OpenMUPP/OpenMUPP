// SPDX-License-Identifier: GPL-3.0-or-later
// The debug log on UART5. See debug/DebugUart.h.
#include "debug/DebugUart.h"

#if defined(MUPP_DEBUG_UART)

#include <algorithm>
#include <cmsis_device.h>
#include <peripherals/Dma.h>
#include <peripherals/GpioPin.h>
#include <peripherals/Rcc.h>
#include <peripherals/Usart.h>

#include "board/Board.h"
#include "board/BoardPins.h"

namespace debug {

namespace {

static_assert(board::current.debugUart.has_value(), "MUPP_DEBUG_UART needs the debugUart of the board definition");
constexpr board::Uart pins = *board::current.debugUart;

constexpr uint32_t baudRate = 115200;

// UART5_TX request (CH32FV2x_V3xRM table 11-2). The JTAG fast path (2), the
// SWO capture (1, 2), the UART bridge (8, 9) and the one-wire engine (10, 11)
// use other channels.
Dma::Channel txDma() {
   return { DMA2, 4 };
}

hw::Usart usart() {
   return hw::Usart(UART5);
}

} // namespace

void DebugUart::start() {
   // Port D has the RX pin. The clock of port C is on (initialize_hardware.cpp).
   Rcc::enableGPIODClock();
   Rcc::enableUART5Clock();
   Rcc::enableDMA2Clock();
   board::gpioPin(pins.tx).configure(hw::GpioPin::Mode::Alternate, hw::GpioPin::OutputType::PushPull,
                                     hw::GpioPin::Pull::None, hw::GpioPin::Speed::High);
   board::gpioPin(pins.rx).configure(hw::GpioPin::Mode::Input, hw::GpioPin::OutputType::PushPull, hw::GpioPin::Pull::Up);

   const Dma::Channel dma = txDma();
   dma.disable();
   dma.configure(const_cast<uint16_t *>(&UART5->DATAR), nullptr, Dma::DataSize::Size8Bits,
                 Dma::Direction::MemoryToPeripheral, true, false);
   dma.setPriority(Dma::ChannelPriority::low);
   dma.setTransferCount(0);

   const hw::Usart uart = usart();
   // 8N1 never fails (only 7 data bits without parity do)
   static_cast<void>(uart.setFrameFormat(hw::Usart::DataBits::Eight, hw::Usart::Parity::None, hw::Usart::StopBits::StopBits_1));
   uart.setBaudRate(baudRate);
   uart.enableTx();
   uart.enableDmaTx();
   uart.enable();
}

std::size_t DebugUart::writeAvailable() const {
   // The count falls to 0 when the DMA has moved the block into the UART
   return txDma().getDataCount() == 0 ? blockSize : 0;
}

std::size_t DebugUart::write(art::span<const uint8_t> data) {
   if(writeAvailable() == 0) {
      return 0;
   }
   const std::size_t count = std::min(data.size(), blockSize);
   std::copy_n(data.data(), count, block_.data());
   const Dma::Channel dma = txDma();
   dma.disable();
   dma.setMemoryAddress(reinterpret_cast<uintptr_t>(block_.data()));
   dma.setTransferCount(static_cast<uint16_t>(count));
   dma.clearGlobalInterruptFlag();
   dma.enable();
   return count;
}

} // namespace debug

#endif
