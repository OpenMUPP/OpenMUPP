#pragma once

// Mock SPI pair for the JTAG fast path (jtag/JtagFastPath.h). It clocks
// the target of MockJtagPins as the hardware does: TCK low, TDI and TMS
// change, TDO is sampled, TCK high. Each SPI bit takes 2^(shift + 1) core
// cycles (the SPI clock is half the core clock).

#include <cstdint>
#include <vector>

#include "MockJtagPins.h"

namespace vsltest {

template <typename Pins>
class MockJtagSpi {
   public:
   [[nodiscard]] uint32_t clockHz() const {
      return 36'000'000;
   }
   void init() {
      ++inits;
   }
   void fini() {
      ++finis;
   }
   void setShift(uint8_t shift) {
      shift_ = shift;
   }

   bool transfer(Pins &pins, const uint8_t *tdi, const uint8_t *tms, uint8_t *tdo, uint32_t bytes) {
      transfers.push_back(bytes);
      // The SPI clock idles high: TCK low here would give an extra rising
      // edge when the pins change to the SPI.
      tckLowAtTransfer = tckLowAtTransfer || !pins.tckLevel();
      tooLong = tooLong || bytes == 0 || bytes > 0xFFFF;
      for(uint32_t i = 0; i < bytes; ++i) {
         uint8_t in = 0;
         for(uint32_t bit = 0; bit < 8; ++bit) {
            pins.tck(false);
            pins.tdi(((tdi[i] >> bit) & 1u) != 0);
            pins.tms(((tms[i] >> bit) & 1u) != 0);
            pins.time += 1u << shift_;
            in |= static_cast<uint8_t>(pins.tdo() << bit);
            pins.tck(true);
            pins.time += 1u << shift_;
         }
         tdo[i] = in;
      }
      return !fail;
   }

   [[nodiscard]] uint8_t shift() const {
      return shift_;
   }

   std::vector<uint32_t> transfers;
   bool tckLowAtTransfer{ false };
   bool tooLong{ false };
   bool fail{ false };
   int inits{ 0 };
   int finis{ 0 };

   private:
   uint8_t shift_{ 0 };
};

} // namespace vsltest
