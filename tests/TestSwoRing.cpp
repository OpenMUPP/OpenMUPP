// SPDX-License-Identifier: GPL-3.0-or-later

#include "swo/SwoRing.h"

#include <array>
#include <doctest/doctest.h>

using Ring = swo::SwoRing<16>;

namespace {

// The DMA of the test: writes bytes into the ring as the circular DMA does
struct FakeDma {
   Ring &ring;
   uint32_t written = 0;

   void write(std::size_t count) {
      for(std::size_t i = 0; i < count; ++i) {
         ring.data()[written % Ring::size] = static_cast<uint8_t>(written);
         ++written;
      }
   }
};

} // namespace

TEST_CASE("SWO baud rate: the USART divider and the real baud rate at 144 MHz") {
   constexpr uint32_t pclk = 144'000'000;
   CHECK(swo::realBaud(pclk, 9'000'000) == 9'000'000);
   CHECK(swo::realBaud(pclk, 4'500'000) == 4'500'000);
   CHECK(swo::realBaud(pclk, 2'000'000) == 2'000'000);
   // 144 MHz / 1250 = 115200
   CHECK(swo::usartDivider(pclk, 115'200) == 1250U);
   CHECK(swo::realBaud(pclk, 115'200) == 115'200);
   // 144 MHz / 21 = 6.857 Mbaud is the nearest divider to 7 Mbaud
   CHECK(swo::usartDivider(pclk, 7'000'000) == 21U);
   CHECK(swo::realBaud(pclk, 7'000'000) == 6'857'143);
   // The limits: BRR 16 (9 Mbaud) to 65514 (2198 baud). 2197 baud rounds to 65543, more than 0xFFFF.
   CHECK(swo::realBaud(pclk, 2198) == 2198);
   CHECK(swo::realBaud(pclk, 2197) == 0);
   CHECK(swo::realBaud(pclk, 10'000'000) == 0);
   CHECK(swo::realBaud(pclk, 0) == 0);
   // No overflow of the rounding near 2^32
   CHECK(swo::realBaud(0xFFFF'FFFFU, 0xFFFF'FFFFU) == 0);
}

TEST_CASE("SWO ring: writeCount follows the halves and the DMA position") {
   // No half done
   CHECK(Ring::writeCount(0, 0) == 0);
   CHECK(Ring::writeCount(0, 5) == 5);
   // HT done
   CHECK(Ring::writeCount(1, 8) == 8);
   CHECK(Ring::writeCount(1, 15) == 15);
   // HT pending: the interrupt did not run yet
   CHECK(Ring::writeCount(0, 8) == 8);
   CHECK(Ring::writeCount(0, 12) == 12);
   // The DMA wrapped: TC done, or TC pending
   CHECK(Ring::writeCount(2, 0) == 16);
   CHECK(Ring::writeCount(1, 0) == 16);
   CHECK(Ring::writeCount(1, 3) == 19);
   // A full counter of the DMA (position 16) is position 0
   CHECK(Ring::writeCount(2, 16) == 16);
   // Many laps: the count wraps at 2^32 with the halves
   CHECK(Ring::writeCount(0x1FFF'FFFFU, 15) == 0xFFFF'FFFFU);
   CHECK(Ring::writeCount(0x1FFF'FFFFU, 0) == 0);
}

TEST_CASE("SWO ring: read gives the bytes in order across the wrap") {
   Ring ring;
   FakeDma dma{ ring };
   auto written = [&] { return dma.written; };
   std::array<uint8_t, 32> out{};
   bool overrun = true;

   CHECK(ring.read(written, out.data(), out.size(), overrun) == 0);
   CHECK_FALSE(overrun);

   dma.write(10);
   CHECK(ring.available(dma.written) == 10);
   REQUIRE(ring.read(written, out.data(), 4, overrun) == 4);
   CHECK_FALSE(overrun);
   CHECK(out[0] == 0);
   CHECK(out[3] == 3);

   // 6 bytes left, then 10 more: 16 bytes, the full ring, across the wrap
   dma.write(10);
   CHECK(ring.available(dma.written) == 16);
   REQUIRE(ring.read(written, out.data(), out.size(), overrun) == 16);
   CHECK_FALSE(overrun);
   for(std::size_t i = 0; i < 16; ++i) {
      CHECK(out[i] == 4 + i);
   }
   CHECK(ring.available(dma.written) == 0);
}

TEST_CASE("SWO ring: an overflow drops the buffer and sets overrun") {
   Ring ring;
   FakeDma dma{ ring };
   auto written = [&] { return dma.written; };
   std::array<uint8_t, 32> out{};
   bool overrun = false;

   dma.write(17);
   CHECK(ring.read(written, out.data(), out.size(), overrun) == 0);
   CHECK(overrun);
   CHECK(ring.available(dma.written) == 0);

   // The next bytes read normally
   dma.write(3);
   REQUIRE(ring.read(written, out.data(), out.size(), overrun) == 3);
   CHECK_FALSE(overrun);
   CHECK(out[0] == 17);
}

TEST_CASE("SWO ring: a DMA write over the bytes during the copy sets overrun") {
   Ring ring;
   FakeDma dma{ ring };
   std::array<uint8_t, 32> out{};
   bool overrun = false;

   dma.write(14);
   int calls = 0;
   // The DMA writes 3 bytes during the copy: byte 0 is written again
   auto written = [&] {
      if(calls++ == 1) {
         dma.write(3);
      }
      return dma.written;
   };
   CHECK(ring.read(written, out.data(), out.size(), overrun) == 0);
   CHECK(overrun);
   CHECK(ring.available(dma.written) == 0);
}

TEST_CASE("SWO ring: DMA writes during the copy that keep the bytes give no overrun") {
   Ring ring;
   FakeDma dma{ ring };
   std::array<uint8_t, 32> out{};
   bool overrun = true;

   dma.write(10);
   int calls = 0;
   auto written = [&] {
      if(calls++ == 1) {
         dma.write(6);
      }
      return dma.written;
   };
   CHECK(ring.read(written, out.data(), out.size(), overrun) == 10);
   CHECK_FALSE(overrun);
   CHECK(ring.available(dma.written) == 6);
}

TEST_CASE("SWO ring: the counts wrap at 2^32") {
   Ring ring;
   FakeDma dma{ ring };
   auto written = [&] { return dma.written; };
   std::array<uint8_t, 32> out{};
   bool overrun = false;

   // Read up to 8 bytes before the wrap of the count
   dma.written = 0xFFFF'FFF8U;
   static_cast<void>(ring.read(written, out.data(), out.size(), overrun));
   CHECK(overrun);
   dma.write(12);
   CHECK(ring.available(dma.written) == 12);
   REQUIRE(ring.read(written, out.data(), out.size(), overrun) == 12);
   CHECK_FALSE(overrun);
   CHECK(out[0] == 0xF8);
   CHECK(out[11] == 0x03);
}
