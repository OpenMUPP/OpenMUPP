// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The UART of the board (board::current.uart) for the UART bridge, on the
// USART driver of system-stm32 or ch32v00x-system. Target only. The board
// must use the default pins of its USART. Only UART7 of the CH32V307 can
// also use its remap pins (PA6, PA7).
//
// STM32: the USART interrupt moves the bytes between the data register and
// the rings of UartBuffers: each received byte into rx, and the bytes of tx
// to the TX pin while the TX interrupt (TXE) is on. startTx() turns the TX
// interrupt on; the interrupt turns itself off when tx is empty.
//
// CH32V307 (UART7): DMA moves the bytes (RX on DMA2 channel 9, TX on
// channel 8), so the CPU gets no interrupt for each byte (issues #20, #33).
// - RX: a circular DMA fills rxDma_. The interrupt of the RX channel copies
//   the new bytes into rx at the half and the end of rxDma_. pollRx() does
//   the same copy, with the interrupts locked. The DMA reads the data register, so the receive
//   errors come only from the status register: one count for each check
//   that finds an error flag (see serviceRx()).
// - TX: startTx() requests the interrupt of the TX channel. The interrupt
//   gives the DMA the next contiguous block of tx, and at the end of the
//   block it commits the block and starts the next one.
// - Single-wire mode (setSingleWire(), only on a board with UART_TX_DIR):
//   uart/SingleWireLine.h sets the pins. The TX channel interrupt calls
//   txStart() before each block. After the last block, it enables the
//   transfer complete interrupt (TC) of UART7, and that interrupt calls
//   txComplete() after the last stop bit.

#include <array>
#include <cstddef>
#include <cstdint>
#include <peripherals/Usart.h>

#include "uart/FrameFormat.h"
#include "uart/SingleWireLine.h"
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
   // Moves the received bytes that the DMA has stored into rx. Call it
   // before the main loop reads rx. Empty without RX DMA (STM32).
   void pollRx();

#if defined(__riscv)
   // Single-wire mode on the board pin UART_TX_DIR (uart/SingleWireLine.h).
   // Only on a board with a TX shifter (Definition::shifters). A change
   // while the UART sends takes effect after the last stop bit.
   void setSingleWire(bool on);
   [[nodiscard]] bool singleWire() const {
      return line_.singleWire();
   }
#endif

   private:
#if defined(__riscv)
   // Size of the circular RX DMA buffer. The half-transfer interrupt comes
   // each 128 bytes: 640 us at 2 Mbaud. The interrupt can come that late
   // before the DMA overwrites bytes that are not copied. A later interrupt
   // (for example the interrupt lock of a debugWIRE byte, 1.28 ms at 7812
   // baud) loses data: serviceRx() counts one overrun for the lap.
   static constexpr std::size_t rxDmaSize = 256;

   // The pins of SingleWireLine (UartPort.cpp)
   struct LinePins {
      hw::Usart &usart;
      void driveTarget(bool on);
      void connectTx(bool on);
      void enableReceiver(bool on);
   };

   void serviceRx();
   void serviceTx();
   void serviceUart();
   // The direct interrupt entries of the DMA channels and of UART7 (see UartPort.cpp)
   static void rxInterruptEntry();
   static void txInterruptEntry();
   static void uartInterruptEntry();
   static UartPort *instance_;

   std::array<uint8_t, rxDmaSize> rxDma_{};
   // The next byte of rxDma_ that serviceRx() copies. Only the interrupt
   // writes it.
   volatile std::size_t rxTail_ = 0;
   // The rxDma_ position at the last counted receive error (serviceRx()).
   std::size_t errorHead_ = rxDmaSize;
   // 0x7F for 7 data bits: the DMA copies the parity bit in bit 7.
   uint8_t dataMask_ = 0xFF;
   // The length of the block that the TX DMA sends. 0: the DMA is idle.
   volatile std::size_t txLength_ = 0;
   // False after stopTx(): the interrupt starts no new block.
   volatile bool txEnabled_ = false;
#else
   void handleInterrupt();
#endif

   hw::Usart usart_;
   Buffers &buffers_;
#if defined(__riscv)
   LinePins linePins_{ usart_ };
   SingleWireLine<LinePins> line_{ linePins_ };
#endif
};

} // namespace uart
