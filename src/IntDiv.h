#pragma once

// Integer division without 64-bit division. The Cortex-M3 divides 32-bit
// numbers in hardware. A 64-bit division or remainder links
// __aeabi_uldivmod and __udivmoddi4 of libgcc (about 850 bytes, #75).
// These functions give the same results as the 64-bit expressions in
// their comments. A 64-bit multiplication is one UMULL instruction and
// links no library code.

#include <cstdint>

namespace intdiv {

// a / b, rounded up. b is not 0.
constexpr uint32_t ceilDiv(uint32_t a, uint32_t b) {
   return a / b + (a % b != 0 ? 1u : 0u);
}

struct Quotient {
   uint32_t quotient;
   uint32_t remainder;
};

// n / d and n % d. The quotient must fit in 32 bits: (n >> 32) < d.
// Binary long division, one quotient bit per step.
constexpr Quotient divide(uint64_t n, uint32_t d) {
   auto remainder = static_cast<uint32_t>(n >> 32);
   auto low = static_cast<uint32_t>(n);
   uint32_t quotient = 0;
   for(int i = 0; i < 32; ++i) {
      // The remainder is less than d, so 33 bits hold the shifted value
      const bool carry = (remainder >> 31) != 0;
      remainder = remainder << 1 | low >> 31;
      low <<= 1;
      quotient <<= 1;
      if(carry || remainder >= d) {
         remainder -= d;
         quotient |= 1u;
      }
   }
   return { quotient, remainder };
}

// uint64_t{ a } * b / c. The result must fit in 32 bits.
constexpr uint32_t mulDiv(uint32_t a, uint32_t b, uint32_t c) {
   return divide(uint64_t{ a } * b, c).quotient;
}

// (uint64_t{ a } * b + c - 1) / c: a * b / c rounded up. The result must fit
// in 32 bits.
constexpr uint32_t mulDivCeil(uint32_t a, uint32_t b, uint32_t c) {
   const Quotient q = divide(uint64_t{ a } * b, c);
   return q.quotient + (q.remainder != 0 ? 1u : 0u);
}

// n % d for a 64-bit n and a 16-bit d, not 0. With n = hi * 2^32 + lo:
// n % d = ((hi % d) * (2^32 % d) + lo % d) % d. Each product is less than
// d * d, so the sum fits in 32 bits.
constexpr uint32_t mod(uint64_t n, uint16_t d) {
   const uint32_t hi = static_cast<uint32_t>(n >> 32) % d;
   const uint32_t lo = static_cast<uint32_t>(n) % d;
   // 2^32 % d, as (2^32 - d) % d
   const uint32_t wrap = (0u - d) % d;
   return (hi * wrap + lo) % d;
}

} // namespace intdiv
