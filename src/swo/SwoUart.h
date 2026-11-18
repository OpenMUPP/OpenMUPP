// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// SWO receiver in UART (NRZ) mode on the CH32V307: USART2 RX on the SWO pin
// of the board (board::current.swo, PA3), with a circular DMA on DMA1
// channel 6 into a 4 KB ring (swo/SwoRing.h). Target only.
//
// - Only the receiver is on. PA2 (USART2 TX) stays a GPIO: it is AUX1 on
//   openmupp-v1.
// - USART2 is on APB1 (144 MHz). The Vref measurement changes only PCLK2,
//   so the baud rate does not change during a measurement.
// - The HT and TC interrupts of the DMA channel count the filled halves of
//   the ring. They only count, so the CPU gets no interrupt for each byte.
//   The count is correct while an interrupt comes less than 2 KB late
//   (2.3 ms at 9 Mbaud).
// - The error flags are sticky until the next start(). The receiver reads
//   the USART status at each read() and status(): an overrun (ORE) or a
//   framing or noise error (FE, NE) sets a flag. An overflow of the ring
//   also sets the overrun flag.
// - The receiver does not change the TDO pin (PB14), which also connects
//   to the SWO line. It must stay an input while SWO runs.
//
// For the DAP_SWO_* commands (CMSIS-DAP personality only, issue #5).

#include <cstddef>
#include <cstdint>

#include "swo/SwoRing.h"

namespace swo {

class SwoUart {
   public:
   static constexpr std::size_t bufferSize = 4096;

   struct Status {
      bool active;
      // An overflow of the ring or a USART overrun (ORE) since start()
      bool overrun;
      // A framing or noise error (FE, NE) since start()
      bool framingError;
      // The bytes to read, at most bufferSize. 0 after an overflow: the next
      // read() empties the ring.
      uint32_t count;
   };

   SwoUart() = default;
   SwoUart(const SwoUart &) = delete;
   SwoUart &operator=(const SwoUart &) = delete;

   // The baud rate that start() gives for a request, without a start. 0:
   // out of range (2198 baud to 9 Mbaud at 144 MHz).
   [[nodiscard]] static uint32_t realBaud(uint32_t baud);

   // Starts the receiver (8N1) at the divider nearest to baud. It empties
   // the ring and clears the error flags. A running receiver starts again.
   // Returns the real baud rate. 0: out of range, and the receiver is off.
   uint32_t start(uint32_t baud);
   // Turns the receiver off. The bytes in the ring stay readable.
   void stop();

   // Copies up to max received bytes into out and returns the count. After
   // an overflow, the ring is empty: the result is 0 and the overrun flag
   // is set.
   std::size_t read(uint8_t *out, std::size_t max);
   [[nodiscard]] Status status();

   private:
   static void dmaInterruptEntry();
   static SwoUart *instance_;

   void serviceDma();
   [[nodiscard]] uint32_t written() const;
   void sampleErrors();

   SwoRing<bufferSize> ring_;
   // HT and TC interrupts since start(). Only the interrupt writes it.
   volatile uint32_t halves_ = 0;
   bool active_ = false;
   bool overrun_ = false;
   bool framingError_ = false;
};

} // namespace swo
