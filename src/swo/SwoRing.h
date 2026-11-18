// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The ring buffer of the SWO receiver (swo/SwoReceiver.h). No CMSIS
// dependency, so the host unit tests can check it.
//
// A circular DMA writes the received bytes into the buffer. The DMA counter
// gives the write position in the buffer, and the half-transfer (HT) and
// transfer-complete (TC) interrupts count the halves that the DMA filled.
// From the two, writeCount() makes a byte count that does not wrap with the
// buffer. The reader keeps its own count. The difference is the number of
// bytes to read. When it is more than the buffer size, the DMA wrote over
// bytes that are not read: an overflow.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace swo {

// The USART divider of the CH32V307 (16x oversampling only): baud = clock /
// BRR. BRR is 16 to 0xFFFF. Empty when the baud rate is out of range.
constexpr std::optional<uint32_t> usartDivider(uint32_t clock, uint32_t baud) {
   if(baud == 0) {
      return std::nullopt;
   }
   const uint64_t divider = (uint64_t{ clock } + baud / 2) / baud;
   if(divider < 16 || divider > 0xFFFF) {
      return std::nullopt;
   }
   return static_cast<uint32_t>(divider);
}

// The baud rate of a divider, rounded. 0 when the request is out of range,
// as DAP_SWO_Baudrate replies.
constexpr uint32_t realBaud(uint32_t clock, uint32_t baud) {
   const auto divider = usartDivider(clock, baud);
   return divider ? static_cast<uint32_t>((uint64_t{ clock } + *divider / 2) / *divider) : 0;
}

template <std::size_t Size>
class SwoRing {
   static_assert(Size >= 2 && (Size & (Size - 1)) == 0, "the counts wrap at 2^32: the size must be a power of 2");

   public:
   static constexpr std::size_t size = Size;
   static constexpr std::size_t half = Size / 2;

   // The bytes that the DMA has written since reset(), modulo 2^32.
   // halves: the HT and TC interrupts that ran. position: the write position
   // of the DMA (Size - counter). Call it with the interrupts locked, so
   // halves and position are from the same moment. The interrupt of the last
   // half can be pending: then halves is one behind position. Limit: the
   // count is wrong if the interrupt comes more than one half (Size / 2
   // bytes) late.
   static constexpr uint32_t writeCount(uint32_t halves, std::size_t position) {
      position %= Size;
      const bool upper = position >= half;
      if(((halves & 1U) != 0) != upper) {
         ++halves;
      }
      return static_cast<uint32_t>(halves * half + position % half);
   }

   void reset() {
      readCount_ = 0;
   }

   // The DMA target
   [[nodiscard]] uint8_t *data() {
      return data_.data();
   }

   // The bytes that are not read. More than Size: an overflow.
   [[nodiscard]] uint32_t available(uint32_t written) const {
      return written - readCount_;
   }

   // Copies up to max bytes into out and returns the count. written() gives
   // the writeCount() now. It is called before the copy and after it: the
   // DMA can write during the copy. If the DMA wrote over bytes that are not
   // read, or over bytes during the copy, the result is 0, the buffer is
   // empty and overrun is true.
   template <typename Written>
   std::size_t read(Written written, uint8_t *out, std::size_t max, bool &overrun) {
      overrun = false;
      const uint32_t start = readCount_;
      const uint32_t before = written();
      const uint32_t pending = before - start;
      if(pending > Size) {
         overrun = true;
         readCount_ = before;
         return 0;
      }
      const std::size_t count = pending < max ? pending : max;
      for(std::size_t i = 0; i < count; ++i) {
         out[i] = data_[(start + i) % Size];
      }
      // The DMA writes byte start + k again at write count start + k + Size.
      const uint32_t after = written();
      if(count != 0 && after - start > Size) {
         overrun = true;
         readCount_ = after;
         return 0;
      }
      readCount_ = start + static_cast<uint32_t>(count);
      return count;
   }

   private:
   std::array<uint8_t, Size> data_{};
   uint32_t readCount_ = 0;
};

} // namespace swo
