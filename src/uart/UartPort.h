#pragma once

// The UART of the board (board::current.uart) for the UART bridge, on the
// system-stm32 USART driver. Target only. The board must use the default
// pins of its USART (no AFIO remap).
//
// The USART interrupt moves the bytes between the data register and the
// rings of UartBuffers: each received byte into rx, and the bytes of tx to
// the TX pin while the TX interrupt (TXE) is on. startTx() turns the TX
// interrupt on; the interrupt turns itself off when tx is empty.

#include <cstdint>
#include <peripherals/Usart.h>

#include "uart/FrameFormat.h"
#include "uart/UartBridge.h"

namespace uart {

class UartPort {
   public:
   using Buffers = UartBuffers<bridgeBufferSize>;

   explicit UartPort(Buffers &buffers);

   UartPort(const UartPort &) = delete;
   UartPort &operator=(const UartPort &) = delete;

   // Turns on the USART clock, sets the TX and RX pins and connects the
   // interrupt. The USART stays off until the first setFormat().
   void start();

   // The UART interface of UartBridge (see uart/UartBridge.h) and of
   // DapUartChannel (uart/DapUartChannel.h). Call them after start().
   [[nodiscard]] uint32_t clockFrequency() const;
   // Sets the frame format and the baud rate, and enables the USART with
   // its transmitter, its receiver and the receive interrupt.
   void setFormat(const FrameFormat &format);
   [[nodiscard]] bool isTxIdle() const;
   void startTx();
   // Turns the TX interrupt off: the interrupt takes no more bytes from
   // tx. The byte in the shift register still goes out. For the owner
   // switch of uart/DapUartChannel.h.
   void stopTx();

   private:
   void handleInterrupt();

   hw::Usart usart_;
   Buffers &buffers_;
};

} // namespace uart
