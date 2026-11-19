// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// SWO receiver on the CH32V307, on the SWO pin of the board
// (board::current.swo, PA3), into a 4 KB ring (swo/SwoRing.h). Target only.
//
// UART (NRZ) mode: USART2 RX, with a circular DMA on DMA1 channel 6.
//
// - Only the receiver is on. PA2 (USART2 TX) stays a GPIO: it is AUX1 on
//   openmupp-v1.
// - USART2 is on APB1 (144 MHz). The Vref measurement changes only PCLK2,
//   so the baud rate does not change during a measurement.
// - The HT and TC interrupts of the DMA channel count the filled halves of
//   the ring. They only count, so the CPU gets no interrupt for each byte.
//   The count is correct while an interrupt comes less than 2 KB late
//   (2.3 ms at 9 Mbaud).
// - The USART status is read at each read() and status(): an overrun
//   (ORE) or a framing or noise error (FE, NE) sets a flag.
//
// Manchester mode: TIM5 captures the edges of PA3 (TIM5_CH4, default map),
// and the CPU decodes them (swo/ManchesterDecoder.h).
//
// - The general-purpose timers of the CH32V307 capture one edge direction
//   per channel (CCER has no CCxNP bit, and TIMx_AUX is only on some
//   lots). So channel 4 captures the rising edges of TI4, and channel 3
//   the falling edges of the same TI4 input (CC3S = 10).
// - TIM5_CH4 is on DMA2 channel 1, TIM5_CH3 on DMA2 channel 2. Each
//   circular DMA writes the 16-bit capture times into a ring of 1024
//   edges. Their HT and TC interrupts count the halves, as in UART mode.
// - DMA2 channel 2 is also the TMS channel of the JTAG fast path
//   (board/BoardJtagFastV30x.cpp). board/SharedDmaChannel.h keeps the two
//   apart: each JTAG setup (all processors, the AVR JTAG target) stops the
//   capture, and a start gives 0 while JTAG has the channel. stop() clears
//   the circular mode and the interrupts of the channel, which the JTAG
//   setup does not clear. TIM5 is on APB1: the Vref measurement does
//   not change its clock.
// - The decoder runs in poll(), in the main loop and in the serial service
//   of a long command, and before each read() and status(). Each call
//   decodes all edges in the rings.
// - The bit rate is at most maxManchesterBaud. That limit comes from the
//   CPU time of the decoder (docs/cmsis-dap.md, "SWO Manchester mode").
//
// Both modes:
//
// - The error flags tell of the errors since the last clearErrors() or
//   start(). An overflow of a ring sets the overrun flag. A Manchester
//   framing error sets the framing error flag.
// - The receiver does not change the TDO pin (PB14), which also connects
//   to the SWO line. It must stay an input while SWO runs.
//
// The receiver of the DAP_SWO_* commands (protocol/cmsisdap/DapSwo.h),
// CMSIS-DAP personality only (issue #5).

#include <cstddef>
#include <cstdint>

#include "protocol/cmsisdap/DapSwo.h"
#include "swo/ManchesterDecoder.h"
#include "swo/SwoRing.h"

namespace swo {

class SwoReceiver final : public cmsisdap::DapSwo {
   public:
   static constexpr std::size_t bufferSize = 4096;
   static constexpr std::size_t edgeBufferSize = 1024;
   // The highest Manchester bit rate (docs/cmsis-dap.md)
   static constexpr uint32_t maxManchesterBaud = 500'000;

   SwoReceiver() = default;
   SwoReceiver(const SwoReceiver &) = delete;
   SwoReceiver &operator=(const SwoReceiver &) = delete;

   [[nodiscard]] uint32_t capacity() const override {
      return bufferSize;
   }
   [[nodiscard]] bool hasMode(uint8_t mode) const override {
      return mode == cmsisdap::swoMode::uart || mode == cmsisdap::swoMode::manchester;
   }
   // The baud rate that start() gives for a request, without a start.
   // UART: 0 below 2198 baud at 144 MHz, the maximum (9 Mbaud) above it.
   // Manchester: the bit rate, 0 below 69 bit/s, maxManchesterBaud above
   // it.
   [[nodiscard]] uint32_t realBaud(uint8_t mode, uint32_t baud) const override;

   // Starts the receiver in a mode. UART: 8N1 at the divider nearest to
   // baud. It empties the ring and clears the error flags. A running
   // receiver starts again. Returns the real baud rate. 0: out of range,
   // and the receiver is off.
   uint32_t start(uint8_t mode, uint32_t baud) override;
   // Changes the baud rate of the running receiver, in its mode.
   uint32_t setBaud(uint32_t baud) override;
   // Turns the receiver off. The bytes in the ring stay readable.
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

   // Manchester mode: decodes the captured edges into the ring. Call it in
   // the main loop, at least each millisecond. Does nothing in UART mode.
   void poll();

   private:
   static void dmaInterruptEntry();
   static void risingInterruptEntry();
   static void fallingInterruptEntry();
   // Stops the capture when the JTAG fast path takes DMA2 channel 2
   static void releaseForJtag();
   static SwoReceiver *instance_;

   uint32_t startUart(uint32_t baud);
   uint32_t startManchester(uint32_t baud);
   void stopManchester();
   [[nodiscard]] uint32_t written() const;
   void sampleErrors();
   void checkLevel(uint32_t rising, uint32_t falling, uint16_t now);

   // The rings are in .bss, not in the object: the object has a vtable, so
   // it is in .data, and the image of .data takes flash. Thus only one
   // receiver can exist (also for instance_).
   static SwoRing<bufferSize> &ring();
   static ManchesterEdges<edgeBufferSize> &edges();

   ManchesterDecoder decoder_;
   // HT and TC interrupts since start(). Only the interrupts write them.
   volatile uint32_t halves_ = 0;
   volatile uint32_t risingHalves_ = 0;
   volatile uint32_t fallingHalves_ = 0;
   // Manchester mode: the bytes that the decoder wrote into ring_
   uint32_t decoded_ = 0;
   // Manchester mode: the edge counts of the last checkLevel(), and a level
   // of the pin that is not the level of the counts
   uint32_t checkedRising_ = 0;
   uint32_t checkedFalling_ = 0;
   bool levelMismatch_ = false;
   uint8_t mode_ = cmsisdap::swoMode::off;
   bool active_ = false;
   bool overrun_ = false;
   bool framingError_ = false;
};

} // namespace swo
