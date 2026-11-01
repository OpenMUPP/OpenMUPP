// SPDX-License-Identifier: GPL-3.0-or-later
// The 32-bit division helpers (IntDiv.h), and the functions that use them,
// against the 64-bit expressions that they replace (#75).

#include "IntDiv.h"

#include "ErrorLed.h"
#include "Heartbeat.h"
#include "UptimeMicros.h"
#include "jtag/JtagBitBang.h"
#include "rvswd/RvSwdEngine.h"
#include "swd/SwdBitBang.h"
#include "uart/FrameFormat.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

namespace {

constexpr uint32_t max32 = std::numeric_limits<uint32_t>::max();
constexpr uint64_t max64 = std::numeric_limits<uint64_t>::max();

// The limits and some values between them
constexpr std::initializer_list<uint32_t> values32{ 0u,          1u,         2u,         3u,
                                                    999u,        1000u,      65535u,     65536u,
                                                    999'999u,    1'000'000u, 8'000'000u, 36'000'000u,
                                                    72'000'000u, 1u << 31,   max32 - 1,  max32 };

// A fixed pseudo-random sequence
struct Lcg {
   uint64_t state{ 0x2545'F491'4F6C'DD1Dull };
   uint64_t next() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return state;
   }
};

// The 64-bit expressions of the code before #75
uint32_t refJtagHalfPeriodCycles(uint32_t coreHz, uint32_t kHz) {
   const uint64_t cyclesPerPeriod = 2ull * kHz * 1000u;
   return static_cast<uint32_t>((coreHz + cyclesPerPeriod - 1) / cyclesPerPeriod);
}

uint32_t refHalfPeriodCyclesOfHz(uint32_t coreHz, uint32_t hz) {
   const uint64_t cyclesPerPeriod = 2ull * (hz == 0 ? 1u : hz);
   return static_cast<uint32_t>((coreHz + cyclesPerPeriod - 1) / cyclesPerPeriod);
}

uint32_t refSwdDelayCycles(uint32_t coreHz, uint16_t delay) {
   return static_cast<uint32_t>((static_cast<uint64_t>(coreHz) * (delay + 1u) + 999'999u) / 1'000'000u);
}

uint32_t refCyclesOfNs(uint32_t coreHz, uint32_t ns) {
   return static_cast<uint32_t>((static_cast<uint64_t>(coreHz) * ns + 999'999'999u) / 1'000'000'000u);
}

std::optional<uint32_t> refBaudRateDivider(uint32_t baudRate, uint32_t uartClock) {
   if(baudRate == 0) {
      return std::nullopt;
   }
   const uint64_t divider = (static_cast<uint64_t>(uartClock) + baudRate / 2) / baudRate;
   if(divider < 16 || divider > 0xFFFF) {
      return std::nullopt;
   }
   const uint64_t real = uartClock;
   const uint64_t wanted = divider * baudRate;
   const uint64_t error = real > wanted ? real - wanted : wanted - real;
   if(error * 100 > wanted * uart::maxBaudErrorPercent) {
      return std::nullopt;
   }
   return static_cast<uint32_t>(divider);
}

uint32_t refUptimeMicroseconds(uint64_t ticks, uint32_t reload, uint32_t value, bool tickPending) {
   const uint64_t elapsed = reload - value;
   if(tickPending && elapsed * 2 <= reload) {
      ++ticks;
   }
   return static_cast<uint32_t>(ticks * 1000 + elapsed * 1000 / reload);
}

} // namespace

TEST_CASE("ceilDiv rounds up") {
   for(const uint32_t a : values32) {
      for(const uint32_t b : values32) {
         if(b == 0) {
            continue;
         }
         CHECK(intdiv::ceilDiv(a, b) == static_cast<uint32_t>((uint64_t{ a } + b - 1) / b));
      }
   }
}

TEST_CASE("divide gives the quotient and the remainder of a 64-bit number") {
   Lcg lcg;
   for(int i = 0; i < 20'000; ++i) {
      const auto d = static_cast<uint32_t>(lcg.next() >> (i % 32));
      if(d == 0) {
         continue;
      }
      // A quotient that fits in 32 bits: the high word is less than d
      const uint64_t n = (lcg.next() >> 32) % d << 32 | (lcg.next() & max32);
      const intdiv::Quotient q = intdiv::divide(n, d);
      REQUIRE(q.quotient == n / d);
      REQUIRE(q.remainder == n % d);
   }
   // The largest quotient and remainder
   const intdiv::Quotient q = intdiv::divide(uint64_t{ max32 - 1 } << 32 | max32, max32);
   CHECK(q.quotient == max32);
   CHECK(q.remainder == max32 - 1);
   static_assert(intdiv::divide(10, 3).quotient == 3 && intdiv::divide(10, 3).remainder == 1);
}

TEST_CASE("mulDiv and mulDivCeil are a * b / c, rounded down and up") {
   for(const uint32_t a : values32) {
      for(const uint32_t b : values32) {
         for(const uint32_t c : values32) {
            const uint64_t product = uint64_t{ a } * b;
            if(c == 0 || (product >> 32) >= c) {
               continue; // the result does not fit in 32 bits
            }
            CHECK(intdiv::mulDiv(a, b, c) == product / c);
            const uint64_t up = product / c + (product % c != 0 ? 1u : 0u);
            if(up <= max32) {
               CHECK(intdiv::mulDivCeil(a, b, c) == up);
            }
         }
      }
   }
}

TEST_CASE("mod is the remainder of a 64-bit number by a 16-bit number") {
   Lcg lcg;
   for(const uint16_t d : { 1, 2, 3, 7, 100, 200, 600, 1000, 4096, 65521, 65534, 65535 }) {
      for(const uint64_t n :
          { uint64_t{ 0 }, uint64_t{ d - 1u }, uint64_t{ d }, uint64_t{ max32 }, uint64_t{ max32 } + 1, max64 - 1, max64 }) {
         CHECK(intdiv::mod(n, d) == n % d);
      }
      for(int i = 0; i < 2000; ++i) {
         const uint64_t n = lcg.next();
         REQUIRE(intdiv::mod(n, d) == n % d);
      }
   }
}

TEST_CASE("The JTAG half period is the same as with 64-bit arithmetic") {
   for(const uint32_t coreHz : values32) {
      for(const uint32_t kHz : values32) {
         if(kHz == 0) {
            continue; // not allowed
         }
         CHECK(jtag::halfPeriodCycles(coreHz, kHz) == refJtagHalfPeriodCycles(coreHz, kHz));
      }
   }
}

TEST_CASE("The SWD and RVSWD half periods are the same as with 64-bit arithmetic") {
   for(const uint32_t coreHz : values32) {
      for(const uint32_t hz : values32) {
         CHECK(swd::halfPeriodCyclesOfHz(coreHz, hz) == refHalfPeriodCyclesOfHz(coreHz, hz));
         CHECK(rvswd::halfPeriodCycles(coreHz, hz) == refHalfPeriodCyclesOfHz(coreHz, hz));
      }
      for(const uint16_t delay : { 0, 1, 5, 6, 7, 100, 1000, 65534, 65535 }) {
         CHECK(swd::halfPeriodCycles(coreHz, delay) == (delay < swd::fixedClockHz.size() ?
                                                        refHalfPeriodCyclesOfHz(coreHz, swd::fixedClockHz[delay]) :
                                                        refSwdDelayCycles(coreHz, delay)));
      }
   }
}

TEST_CASE("The RVSWD cycles of a time are the same as with 64-bit arithmetic") {
   for(const uint32_t coreHz : values32) {
      for(const uint32_t ns : { 0u, 1u, 100u, 999u, 4'000u, 5'000u, 1'000'000u, 2'000'000u, 999'999'999u }) {
         CHECK(rvswd::cyclesOfNs(coreHz, ns) == refCyclesOfNs(coreHz, ns));
      }
   }
}

TEST_CASE("The UART divider is the same as with 64-bit arithmetic") {
   for(const uint32_t uartClock : values32) {
      for(const uint32_t baud : { 0u, 1u, 1098u, 1099u, 9600u, 115'200u, 921'600u, 2'880'000u, 3'500'000u, 3'600'000u,
                                  4'500'000u, 4'500'001u, 1u << 31, max32 }) {
         CHECK(uart::baudRateDivider(baud, uartClock) == refBaudRateDivider(baud, uartClock));
      }
   }
   // Each baud rate at the limits of the 72 MHz and 36 MHz UART clocks
   for(const uint32_t uartClock : { 36'000'000u, 72'000'000u }) {
      for(uint32_t baud = uartClock / 0xFFFF - 2; baud < uartClock / 0xFFFF + 2000; ++baud) {
         REQUIRE(uart::baudRateDivider(baud, uartClock) == refBaudRateDivider(baud, uartClock));
      }
      for(uint32_t baud = uartClock / 16 - 2000; baud < uartClock / 16 + 2; ++baud) {
         REQUIRE(uart::baudRateDivider(baud, uartClock) == refBaudRateDivider(baud, uartClock));
      }
   }
}

TEST_CASE("The uptime is the same as with 64-bit arithmetic") {
   for(const uint64_t ticks : { uint64_t{ 0 }, uint64_t{ 5 }, uint64_t{ max32 }, uint64_t{ max32 } + 1, max64 - 1 }) {
      for(const uint32_t reload : { 1u, 2u, 3u, 72'000u, 1u << 24 }) {
         for(const uint32_t value : { 0u, 1u, reload / 2 - (reload > 1 ? 1u : 0u), reload / 2, reload - 1 }) {
            for(const bool pending : { false, true }) {
               CHECK(uptimeMicroseconds(ticks, reload, value, pending) == refUptimeMicroseconds(ticks, reload, value, pending));
            }
         }
      }
   }
}

TEST_CASE("The LED patterns are the same as with a 64-bit remainder") {
   constexpr Heartbeat heartbeat{ Heartbeat::clockFault };
   for(const uint64_t start : { uint64_t{ 0 }, uint64_t{ max32 } - 300, uint64_t{ 1 } << 40 }) {
      ErrorLed led;
      led.update(start, 1);
      for(uint64_t t = start; t < start + 1500; ++t) {
         led.update(t, 1);
         const uint64_t phase = (t - start) % ErrorLed::periodMs;
         REQUIRE(led.isOn() == (phase < ErrorLed::flashMs || (phase >= 2 * ErrorLed::flashMs && phase < 3 * ErrorLed::flashMs)));
         REQUIRE(heartbeat.isOn(t) == (t % Heartbeat::clockFault.periodMs < Heartbeat::clockFault.onMs));
      }
   }
}
