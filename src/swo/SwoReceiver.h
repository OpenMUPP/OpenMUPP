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
// - The error flags tell of the errors since the last clearErrors() or
//   start().
//   The receiver reads the USART status at each read() and status(): an
//   overrun (ORE) or a framing or noise error (FE, NE) sets a flag. An
//   overflow of the ring also sets the overrun flag.
// - The receiver does not change the TDO pin (PB14), which also connects
//   to the SWO line. It must stay an input while SWO runs.
//
// The receiver of the DAP_SWO_* commands (protocol/cmsisdap/DapSwo.h),
// CMSIS-DAP personality only (issue #5).

#include <cstddef>
#include <cstdint>

#include "protocol/cmsisdap/DapSwo.h"
#include "swo/SwoRing.h"

namespace swo {

class SwoReceiver final : public cmsisdap::DapSwo {
   public:
   static constexpr std::size_t bufferSize = 4096;

   SwoReceiver() = default;
   SwoReceiver(const SwoReceiver &) = delete;
   SwoReceiver &operator=(const SwoReceiver &) = delete;

   [[nodiscard]] uint32_t capacity() const override {
      return bufferSize;
   }
   [[nodiscard]] bool hasMode(uint8_t mode) const override {
      return mode == cmsisdap::swoMode::uart;
   }
   // The baud rate that start() gives for a request, without a start. 0:
   // out of range (below 2198 baud at 144 MHz). A request above the
   // maximum (9 Mbaud) gives the maximum.
   [[nodiscard]] uint32_t realBaud(uint8_t mode, uint32_t baud) const override;

   // Starts the receiver (8N1) at the divider nearest to baud. It empties
   // the ring and clears the error flags. A running receiver starts again.
   // Returns the real baud rate. 0: out of range, and the receiver is off.
   uint32_t start(uint8_t mode, uint32_t baud) override;
   // Turns the receiver off. The bytes in the ring stay readable.
   uint32_t setBaud(uint32_t baud) override;
   void stop() override;
   [[nodiscard]] bool active() const override {
      return active_;
   }

   // Copies up to data.size() received bytes and returns the count. After
   // an overflow, the ring is empty: the result is 0 and the overrun flag
   // is set.
   [[nodiscard]] std::size_t read(art::span<uint8_t> data) override;
   // count is at most bufferSize. It is 0 after an overflow: the next
   // read() empties the ring.
   [[nodiscard]] Status status() override;
   void clearErrors() override {
      overrun_ = false;
      framingError_ = false;
   }

   private:
   static void dmaInterruptEntry();
   static SwoReceiver *instance_;

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
