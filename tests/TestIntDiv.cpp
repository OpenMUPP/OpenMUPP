// The 32-bit division helpers (IntDiv.h) against 64-bit arithmetic (#75).

#include "IntDiv.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <limits>

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
