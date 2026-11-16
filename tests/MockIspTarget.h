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
//
// In the programming mode, the target has the memories of an ATmega2560
// (256 KB flash with 128-word pages and the extended address, 4 KB EEPROM
// with 8-byte pages, fuses, lock, calibration) and executes the serial
// programming instructions of the datasheet. A write makes the target busy
// for busyPolls polls: RDY/BSY reads 1, and a read of the memory reads
// 0xFF. A write programs only the bits that are 1 (flash and EEPROM), as
// the target does without an erase.

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
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
         // The extended address register of the AVR starts at 0.
         extended = 0;
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
      if(index == 2 && enabled) {
         const int value = readValue();
         if(value >= 0) {
            pendingOut_ = value;
         }
      }
      if(index == 3) {
         framePos_ = 0;
         frames.push_back(frame_);
         if(enabled) {
            execute();
         }
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

   static constexpr uint32_t flashBytes = 256u * 1024u;
   static constexpr uint32_t pageWords = 128;
   static constexpr uint32_t eepromBytes = 4096;
   static constexpr uint32_t eepromPageBytes = 8;

   uint32_t syncOffset{ 0 };
   std::array<uint8_t, 3> signature{ { 0x1E, 0x95, 0x0F } };
   std::vector<uint8_t> flash = std::vector<uint8_t>(flashBytes, 0xFF);
   std::vector<uint8_t> eeprom = std::vector<uint8_t>(eepromBytes, 0xFF);
   uint8_t lowFuse{ 0x62 };
   uint8_t highFuse{ 0xD9 };
   uint8_t extendedFuse{ 0xFF };
   uint8_t lock{ 0xFF };
   uint8_t calibration{ 0xA5 };
   // The extended address byte (instruction 0x4D)
   uint8_t extended{ 0 };
   int busyPolls{ 0 };
   int chipErases{ 0 };
   // The word address of each Write Program Memory Page
   std::vector<uint32_t> pageWrites;
   bool enabled{ false };
   bool sckHighAtReset{ false };
   int resets{ 0 };
   std::vector<std::array<uint8_t, 4>> frames;

   private:
   [[nodiscard]] uint32_t flashIndex() const {
      const uint32_t word = (uint32_t{ extended } << 16) | (uint32_t{ frame_[1] } << 8) | frame_[2];
      return (2u * word + ((frame_[0] & 0x08u) != 0 ? 1u : 0u)) % flashBytes;
   }

   [[nodiscard]] uint32_t eepromIndex() const {
      return ((uint32_t{ frame_[1] } << 8) | frame_[2]) % eepromBytes;
   }

   // A memory read while the target is busy reads 0xFF.
   int busyOr(uint8_t value) {
      if(busy_ > 0) {
         --busy_;
         return 0xFF;
      }
      return value;
   }

   // The response byte 4 of a read instruction, -1 for other instructions
   int readValue() {
      const uint8_t a = frame_[0];
      const uint8_t b = frame_[1];
      switch(a) {
      case 0x20:
      case 0x28:
         return busyOr(flash[flashIndex()]);
      case 0xA0:
         return busyOr(eeprom[eepromIndex()]);
      case 0x30:
         return signature[frame_[2] & 3u];
      case 0x38:
         return calibration;
      case 0x50:
         return b == 0x08 ? extendedFuse : lowFuse;
      case 0x58:
         return b == 0x08 ? highFuse : lock;
      case 0xF0:
         if(busy_ > 0) {
            --busy_;
            return 1;
         }
         return 0;
      default:
         return -1;
      }
   }

   void execute() {
      const uint8_t a = frame_[0];
      const uint8_t b = frame_[1];
      const uint8_t data = frame_[3];
      switch(a) {
      case 0x4D:
         extended = frame_[2];
         break;
      case 0x40:
      case 0x48: {
         const uint32_t word = ((uint32_t{ b } << 8) | frame_[2]) % pageWords;
         pageBuffer_[2u * word + (a == 0x48 ? 1u : 0u)] = data;
         break;
      }
      case 0x4C: {
         const uint32_t page = (flashIndex() / 2u) & ~(pageWords - 1u);
         for(uint32_t i = 0; i < 2u * pageWords; ++i) {
            flash[2u * page + i] &= pageBuffer_[i];
         }
         pageBuffer_.fill(0xFF);
         pageWrites.push_back(page);
         busy_ = busyPolls;
         break;
      }
      case 0x60:
      case 0x68:
         flash[flashIndex()] &= data;
         busy_ = busyPolls;
         break;
      case 0xC0:
         eeprom[eepromIndex()] &= data;
         busy_ = busyPolls;
         break;
      case 0xC1:
         eepromLoads_.emplace_back(eepromIndex() % eepromPageBytes, data);
         break;
      case 0xC2: {
         const uint32_t page = eepromIndex() & ~(eepromPageBytes - 1u);
         for(const auto &[offset, value] : eepromLoads_) {
            eeprom[page + offset] &= value;
         }
         eepromLoads_.clear();
         busy_ = busyPolls;
         break;
      }
      case 0xAC:
         executeAc(b, data);
         break;
      default:
         break;
      }
   }

   void executeAc(uint8_t b, uint8_t data) {
      switch(b) {
      case 0x80:
         std::fill(flash.begin(), flash.end(), 0xFF);
         std::fill(eeprom.begin(), eeprom.end(), 0xFF);
         lock = 0xFF;
         ++chipErases;
         busy_ = busyPolls;
         break;
      case 0xA0:
         lowFuse = data;
         break;
      case 0xA8:
         highFuse = data;
         break;
      case 0xA4:
         extendedFuse = data;
         break;
      case 0xE0:
         lock = data;
         break;
      default:
         break;
      }
   }

   bool inReset_{ false };
   bool sck_{ false };
   uint32_t framePos_{ 0 };
   uint8_t shiftIn_{ 0 };
   uint8_t out_{ 0 };
   int pendingOut_{ -1 };
   std::array<uint8_t, 4> frame_{};
   std::array<uint8_t, 2u * pageWords> pageBuffer_ = [] {
      std::array<uint8_t, 2u * pageWords> buffer{};
      buffer.fill(0xFF);
      return buffer;
   }();
   std::vector<std::pair<uint32_t, uint8_t>> eepromLoads_;
   int busy_{ 0 };
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
