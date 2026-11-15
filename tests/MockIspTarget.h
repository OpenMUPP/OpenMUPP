// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A mock AVR for the ISP engine (avr/IspEngine.h), with mock pins and a
// mock SPI that clock it.
//
// The target samples MOSI at the rising edge of SCK and changes MISO at
// the falling edge (SPI mode 0, MSB first). While RESET is asserted, it
// receives 4-byte frames. Each response byte is the byte before it (the
// echo of the AVR), and byte 4 of a read instruction is the data. A frame
// 0xAC 0x53 enables the programming mode. Until then, it ignores all other
// instructions and sends 0, except the echo 0x53 of a programming enable
// in sync.
//
// syncOffset: the frame bits that the target counts as received at the
// assert of RESET. With an offset that is not 0, the frames of the target
// and of the engine are not aligned. Each SCK pulse between frames moves
// them by one bit.

#include <array>
#include <cstdint>
#include <vector>

namespace mupptest {

class MockIspTarget {
   public:
   void reset(bool asserted) {
      if(asserted == inReset_) {
         return;
      }
      inReset_ = asserted;
      if(asserted) {
         sckHighAtReset = sckHighAtReset || sck_;
         framePos_ = syncOffset % 32u;
         shiftIn_ = 0;
         out_ = 0;
         pendingOut_ = -1;
         enabled = false;
         ++resets;
      }
   }

   void rising(bool mosi) {
      sck_ = true;
      if(!inReset_) {
         return;
      }
      shiftIn_ = static_cast<uint8_t>((shiftIn_ << 1) | (mosi ? 1u : 0u));
      ++framePos_;
      if(framePos_ % 8u != 0) {
         return;
      }
      const uint32_t index = framePos_ / 8u - 1u;
      frame_[index] = shiftIn_;
      // Before the programming enable, the target answers only the
      // programming enable in sync: 0x53 in the third byte.
      pendingOut_ = enabled ? shiftIn_ : 0;
      if(!enabled && index == 1 && frame_[0] == 0xAC && shiftIn_ == 0x53) {
         pendingOut_ = 0x53;
      }
      if(index == 2 && enabled && frame_[0] == 0x30) {
         pendingOut_ = signature[frame_[2] & 3u];
      }
      if(index == 3) {
         framePos_ = 0;
         frames.push_back(frame_);
         if(frame_[0] == 0xAC && frame_[1] == 0x53) {
            enabled = true;
         }
      }
   }

   void falling() {
      sck_ = false;
      if(pendingOut_ >= 0) {
         out_ = static_cast<uint8_t>(pendingOut_);
         pendingOut_ = -1;
      } else {
         out_ = static_cast<uint8_t>(out_ << 1);
      }
   }

   // An unselected target does not drive MISO: the pull-up reads 1.
   [[nodiscard]] bool miso() const {
      return !inReset_ || (out_ & 0x80u) != 0;
   }

   [[nodiscard]] bool inReset() const {
      return inReset_;
   }

   uint32_t syncOffset{ 0 };
   std::array<uint8_t, 3> signature{ { 0x1E, 0x95, 0x0F } };
   bool enabled{ false };
   bool sckHighAtReset{ false };
   int resets{ 0 };
   std::vector<std::array<uint8_t, 4>> frames;

   private:
   bool inReset_{ false };
   bool sck_{ false };
   uint32_t framePos_{ 0 };
   uint8_t shiftIn_{ 0 };
   uint8_t out_{ 0 };
   int pendingOut_{ -1 };
   std::array<uint8_t, 4> frame_{};
};

// The pins of the engine on a MockIspTarget. Each now() call is one time
// step.
class MockIspPins {
   public:
   explicit MockIspPins(MockIspTarget &target)
      : target_(target) {
   }

   void init() {
      ++inits;
      driven = true;
      sck_ = false;
      mosi_ = false;
      reset(true);
   }
   void fini() {
      ++finis;
      driven = false;
      reset(false);
   }
   void begin() {
      ++begins;
   }

   void sck(bool level) {
      if(level == sck_) {
         return;
      }
      sck_ = level;
      if(level) {
         risingTimes.push_back(time);
         target_.rising(mosi_);
      } else {
         target_.falling();
      }
   }
   void mosi(bool level) {
      mosiChangedWhileSckHigh = mosiChangedWhileSckHigh || (sck_ && level != mosi_);
      mosi_ = level;
   }
   [[nodiscard]] bool miso() const {
      return target_.miso();
   }
   void reset(bool asserted) {
      if(asserted && !target_.inReset()) {
         assertTimes.push_back(time);
      }
      target_.reset(asserted);
   }

   uint32_t now() {
      return time++;
   }
   [[nodiscard]] uint32_t coreHz() const {
      return 72'000'000;
   }

   [[nodiscard]] bool sckLevel() const {
      return sck_;
   }

   // Starts near the wrap of the counter.
   uint32_t time{ 0xFFFFFF00u };
   bool driven{ false };
   bool mosiChangedWhileSckHigh{ false };
   std::vector<uint32_t> risingTimes;
   std::vector<uint32_t> assertTimes;
   int inits{ 0 };
   int finis{ 0 };
   int begins{ 0 };

   private:
   MockIspTarget &target_;
   bool sck_{ false };
   bool mosi_{ false };
};

// The SPI of the engine: clocks the target through the pins, as the SPI
// in mode 0 does. Each SPI bit takes 2^shift core cycles.
class MockIspSpi {
   public:
   [[nodiscard]] uint32_t clockHz() const {
      return 72'000'000;
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

   bool transfer(MockIspPins &pins, const uint8_t *out, uint8_t *in, uint32_t bytes) {
      transfers.push_back(bytes);
      sckHighAtTransfer = sckHighAtTransfer || pins.sckLevel();
      for(uint32_t i = 0; i < bytes; ++i) {
         uint8_t value = 0;
         for(uint32_t bit = 0; bit < 8; ++bit) {
            pins.mosi(((out[i] >> (7u - bit)) & 1u) != 0);
            pins.time += 1u << (shift_ - 1u);
            value = static_cast<uint8_t>((value << 1) | (pins.miso() ? 1u : 0u));
            pins.sck(true);
            pins.time += 1u << (shift_ - 1u);
            pins.sck(false);
         }
         in[i] = value;
      }
      return !fail;
   }

   [[nodiscard]] uint8_t shift() const {
      return shift_;
   }

   std::vector<uint32_t> transfers;
   bool sckHighAtTransfer{ false };
   bool fail{ false };
   int inits{ 0 };
   int finis{ 0 };

   private:
   uint8_t shift_{ 2 };
};

} // namespace mupptest
