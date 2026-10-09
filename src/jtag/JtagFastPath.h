#pragma once

// JTAG fast path: shifts the whole bytes of a JTAG_RAW shift with two SPI
// peripherals and DMA, and the other bits with the bit-bang engine
// (jtag/JtagBitBang.h). Hardware-independent: an Spi type gives the SPI
// and DMA access, so the host unit tests run the logic on a mock. On
// target, board::JtagSpi (board/BoardJtag.h, board/BoardJtagFast.cpp)
// gives the SPI1, SPI2 and DMA1 drivers.
//
// The SPI master drives TCK and TDI and samples TDO. An SPI slave, clocked
// by TCK through a board link, drives TMS. Both use clock polarity high and
// the second clock edge: TDI and TMS change at the falling edge, and the
// target and the master sample at the rising edge. The SPI clock idles
// high, so the bit-bang engine idles TCK high too (TckIdle::High).
//
// The Spi type is default-constructible and has these members:
//
//   uint32_t clockHz();       SPI input clock: TCK = clockHz() >> shift
//   void init();              clocks and configuration; the pins stay GPIO
//   void fini();              SPI and DMA off
//   void setShift(uint8_t);   TCK = clockHz() >> shift, shift 2-8
//   bool transfer(Pins &pins, const uint8_t *tdi, const uint8_t *tms,
//                 uint8_t *tdo, uint32_t bytes);
//                             1-65535 whole bytes, LSB first. TCK is high
//                             before and after, and the pins are GPIO
//                             before and after. False when the DMA does
//                             not end in time (a hardware fault). The
//                             pins give the cycle counter (now()).
//
// Speed: the engine with the faster TCK wins, and TCK is never faster than
// the request. The SPI gives clockHz() / 2^n, n = 2-8. Bit-bang gives the
// request, up to bitBangMaxKHz. kHz = 0 (adaptive clocking with RTCK) is
// always bit-bang.
//
// A long shift calls the Background between SPI chunks of about
// backgroundIntervalMs (protocol/Background.h), with TCK high.

#include <algorithm>
#include <cstdint>

#include "jtag/JtagBitBang.h"
#include "protocol/Background.h"
#include "protocol/JtagRawBlock.h"

namespace jtag {

// The SPI clock divider is 2^shift (SPI_CR1.BR + 1). Shift 1 (18 MHz) is
// off: TMS comes from the SPI1 slave, valid up to 25 ns after the falling
// edge (tv(SO), STM32F103x8/B datasheet), and the half period is 27.8 ns.
// Set it to 1 only after a hardware test at 18 MHz passes.
inline constexpr uint8_t spiMinShift = 2;
inline constexpr uint8_t spiMaxShift = 8;

// The TCK rate that the bit-bang loop surely reaches at 72 MHz. Not the
// fastest rate of the loop: that rate is not measured. 1000 kHz gave the same
// OpenOCD flash times as the original firmware (tools/flash_benchmark.py).
// Too high a value selects bit-bang at a speed where the SPI is faster. Too
// low a value selects the SPI, which is a little slower than bit-bang could
// be. Measure TCK at a request of 4000 kHz to raise it.
inline constexpr uint32_t bitBangMaxKHz = 1000;

// The SPI divider shift for a request of kHz, or 0 for bit-bang: the SPI
// rate must not be above the request and must be at least the bit-bang
// rate.
constexpr uint8_t spiShiftFor(uint32_t spiClockHz, uint32_t kHz) {
   if(kHz == 0) {
      return 0;
   }
   const uint64_t requestHz = uint64_t{ kHz } * 1000u;
   const uint64_t bitBangHz = uint64_t{ std::min(kHz, bitBangMaxKHz) } * 1000u;
   for(uint8_t shift = spiMinShift; shift <= spiMaxShift; ++shift) {
      const uint64_t spiHz = spiClockHz >> shift;
      if(spiHz <= requestHz) {
         return spiHz >= bitBangHz ? shift : 0;
      }
   }
   return 0;
}

// The bytes of one SPI transfer: about backgroundIntervalMs of TCK, at
// least 1 and at most the 16-bit DMA count.
constexpr uint32_t spiChunkBytes(uint32_t spiClockHz, uint8_t shift) {
   const uint32_t bytes = (spiClockHz >> shift) / 8u / 1000u * protocol::backgroundIntervalMs;
   return std::clamp<uint32_t>(bytes, 1, 0xFFFF);
}

// The JTAG port of the JTAG_RAW block with the SPI fast path.
template <typename Pins, typename Spi>
class FastPort final : public protocol::JtagPort {
   public:
   // background: serviced during a shift, nullptr for none.
   explicit FastPort(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , bitBang_(pins, background)
      , background_(background) {
   }

   void init() override {
      bitBang_.init();
      spi_.init();
   }

   void fini() override {
      spi_.fini();
      bitBang_.fini();
   }

   [[nodiscard]] bool setSpeed(uint32_t kHz) override {
      if(!bitBang_.setSpeed(kHz)) {
         return false;
      }
      shift_ = spiShiftFor(spi_.clockHz(), kHz);
      if(shift_ != 0) {
         spi_.setShift(shift_);
      }
      return true;
   }

   [[nodiscard]] bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) override {
      const uint32_t bytes = shift_ == 0 ? 0 : bitCount / 8;
      if(bytes > 0) {
         pins_.begin();
         background_.begin(pins_);
         const uint32_t chunk = spiChunkBytes(spi_.clockHz(), shift_);
         for(uint32_t done = 0; done < bytes;) {
            // TCK is high. The bit-bang engine has its own timer, so a
            // shift with a bit-bang tail can call the Background twice in
            // one interval.
            static_cast<void>(background_.poll(pins_));
            const uint32_t count = std::min(chunk, bytes - done);
            if(!spi_.transfer(pins_, tdi.data() + done, tms.data() + done, tdo.data() + done, count)) {
               return false;
            }
            done += count;
         }
      }
      const uint32_t rest = bitCount - bytes * 8;
      if(rest == 0) {
         return true;
      }
      const std::size_t tail = protocol::jtagRaw::byteCount(rest);
      return bitBang_.shift(rest, tdi.subspan(bytes, tail), tms.subspan(bytes, tail), tdo.subspan(bytes, tail));
   }

   // The SPI divider shift, 0 for bit-bang
   [[nodiscard]] uint8_t spiShift() const {
      return shift_;
   }

   [[nodiscard]] const Timing &timing() const {
      return bitBang_.timing();
   }

   [[nodiscard]] Spi &spi() {
      return spi_;
   }

   private:
   Pins &pins_;
   Spi spi_;
   BitBangPort<Pins, TckIdle::High> bitBang_;
   protocol::BackgroundTimer background_;
   // Until INIT sets the speed: bit-bang at its fastest rate, as BitBangPort
   uint8_t shift_{ 0 };
};

} // namespace jtag
