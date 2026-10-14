#pragma once

// A mock flash controller (FPEC) of a CH32V00x or CH32V20x, on the hooks of
// a MockDebugModule.
// It models what the WCH-Link flash module (WchLinkFlash.h) uses:
//
// - The registers KEYR, OBKEYR, MODEKEYR, STATR, CTLR, OBR, WPR. The key
//   sequences clear LOCK, FLOCK and set OBWRE. A key write while the lock
//   is open locks the controller until the next reset, as the reference
//   manuals say.
// - CTLR with STRT: MER erases the main flash, OBER the option bytes.
//   OBWRE is cleared by a CTLR write with OBWRE 0.
// - With OBPG: a half-word write to the option bytes programs the low byte
//   and its inverse. The half word must be erased. Any other write gives an
//   error. RDPR 0xa5 on a protected chip erases the main flash.
// - A system reset (ndmreset) locks the controller and loads OBR and WPR
//   from the option bytes.
//
// A wrong access does not stop the test: it goes to errors.

#include "MockDebugModule.h"
#include "protocol/wchlink/WchLinkFlash.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vsltest {

class MockWchFlash {
   public:
   static constexpr uint32_t flashBase = 0x0800'0000;

   struct Chip {
      uint32_t size;
      uint32_t pageSize;
      // Erased flash reads this word (CH32V208: 0xe339e339).
      uint32_t erasedWord;
      std::size_t optionBytes;
   };
   // The flash sizes are small to keep the tests fast.
   static constexpr Chip v003{ 16 * 1024, 64, 0xffff'ffff, wchlink::fpec::optionBytesV00x };
   static constexpr Chip v208{ 32 * 1024, 256, 0xe339'e339, wchlink::fpec::optionBytesV20x };

   MockWchFlash(MockDebugModule &dm, const Chip &chip)
      : flash(chip.size)
      , dm_(dm)
      , chip_(chip) {
      eraseMain();
      options.fill(0xffff);
      setOption(0, wchlink::fpec::rdprUnprotected);
      for(std::size_t i = 1; i < chip_.optionBytes; ++i) {
         setOption(i, 0xff);
      }
      systemReset();
      dm_.onLoad = [this](uint32_t address, uint32_t bytes) { return load(address, bytes); };
      dm_.onStore = [this](uint32_t address, uint32_t value, uint32_t bytes) { return store(address, value, bytes); };
      dm_.onSystemReset = [this] { systemReset(); };
   }

   // Sets option byte i (half word i) to the value and its inverse.
   void setOption(std::size_t i, uint8_t value) {
      options[i] = static_cast<uint16_t>(value | (~value & 0xff) << 8);
   }
   [[nodiscard]] uint8_t option(std::size_t i) const {
      return static_cast<uint8_t>(options[i]);
   }

   [[nodiscard]] uint32_t word(uint32_t address) const {
      uint32_t value = 0;
      for(uint32_t i = 4; i > 0; --i) {
         value = value << 8 | flash[address - flashBase + i - 1];
      }
      return value;
   }
   void fill(uint32_t address, const std::vector<uint8_t> &bytes) {
      std::copy(bytes.begin(), bytes.end(), flash.begin() + (address - flashBase));
   }
   [[nodiscard]] bool erased(uint32_t address, uint32_t length) const {
      for(uint32_t i = 0; i < length; i += 4) {
         if(word(address + i) != chip_.erasedWord) {
            return false;
         }
      }
      return true;
   }

   // State
   std::vector<uint8_t> flash;
   std::array<uint16_t, wchlink::fpec::optionBytesV20x> options{};
   bool locked{ true };
   bool flocked{ true };
   bool obwre{ false };
   // The operation bits of CTLR (no LOCK, FLOCK, OBWRE, STRT)
   uint32_t mode{ 0 };
   uint32_t status{ 0 };
   bool readProtected{ false };
   uint32_t wpr{ 0xffff'ffff };
   // The STATR reads with BSY after each operation
   uint32_t busyReads{ 1 };

   int massErases{ 0 };
   int autoErases{ 0 };
   int optionErases{ 0 };
   std::vector<std::string> errors;

   private:
   static constexpr uint32_t opMask = 0x00ff'0000u | 0x3fu;

   void eraseMain() {
      for(uint32_t i = 0; i < chip_.size; i += 4) {
         setWord(flashBase + i, chip_.erasedWord);
      }
   }

   void setWord(uint32_t address, uint32_t value) {
      for(uint32_t i = 0; i < 4; ++i) {
         flash[address - flashBase + i] = static_cast<uint8_t>(value >> (8 * i));
      }
   }

   [[nodiscard]] bool inFlash(uint32_t address) const {
      return address >= flashBase && address < flashBase + chip_.size;
   }

   [[nodiscard]] bool inOptions(uint32_t address) const {
      return address >= wchlink::fpec::optionBytes && address < wchlink::fpec::optionBytes + 2 * wchlink::fpec::optionBytesV20x;
   }

   void systemReset() {
      locked = true;
      flocked = true;
      obwre = false;
      mode = 0;
      keySteps_ = {};
      readProtected = option(wchlink::fpec::optionRdpr) != wchlink::fpec::rdprUnprotected;
      wpr = 0;
      for(std::size_t i = 0; i < 4; ++i) {
         const std::size_t index = wchlink::fpec::optionWrpr0 + i;
         const uint32_t byte = index < chip_.optionBytes ? option(index) : 0xff;
         wpr |= byte << (8 * i);
      }
   }

   std::optional<uint32_t> load(uint32_t address, uint32_t bytes) {
      namespace fpec = wchlink::fpec;
      if(inFlash(address)) {
         uint32_t value = 0;
         for(uint32_t i = bytes; i > 0; --i) {
            value = value << 8 | flash[address - flashBase + i - 1];
         }
         return value;
      }
      if(inOptions(address)) {
         const std::size_t index = (address - fpec::optionBytes) / 2;
         uint32_t value = options[index];
         if(bytes == 4) {
            value |= static_cast<uint32_t>(options[index + 1]) << 16;
         }
         return value;
      }
      switch(address) {
      case fpec::ctlr:
         return mode | (locked ? fpec::lock : 0) | (flocked ? fpec::flock : 0) | (obwre ? fpec::obwre : 0);
      case fpec::statr:
         if(busyLeft_ > 0) {
            --busyLeft_;
            return status | fpec::bsy;
         }
         return status;
      case fpec::obr:
         return 0x0000'00fcu | (readProtected ? 2u : 0u);
      case fpec::wpr:
         return wpr;
      default:
         return std::nullopt;
      }
   }

   // A key sequence: KEY1, then KEY2. Returns true when it is complete.
   bool keySequence(int &step, uint32_t value, const char *name) {
      if(step == 0 && value == wchlink::fpec::key1) {
         step = 1;
         return false;
      }
      if(step == 1 && value == wchlink::fpec::key2) {
         step = 0;
         return true;
      }
      errors.push_back(std::string("wrong key sequence: ") + name);
      step = 2;
      return false;
   }

   void operationDone() {
      status |= wchlink::fpec::eop;
      busyLeft_ = busyReads;
   }

   bool store(uint32_t address, uint32_t value, uint32_t bytes) {
      namespace fpec = wchlink::fpec;
      if(inFlash(address)) {
         errors.push_back("store to the main flash");
         return true;
      }
      if(inOptions(address)) {
         storeOption(address, value, bytes);
         return true;
      }
      if(address < 0x4002'2000 || address >= 0x4002'2030) {
         return false;
      }
      if(bytes != 4) {
         errors.push_back("FPEC access that is not a word");
         return true;
      }
      if(busyLeft_ > 0 && address != fpec::statr) {
         errors.push_back("FPEC write while busy");
      }
      switch(address) {
      case fpec::keyr:
         if(!locked) {
            errors.push_back("KEYR write while unlocked");
            keySteps_.key = 2;
         }
         if(keySequence(keySteps_.key, value, "KEYR")) {
            locked = false;
         }
         break;
      case fpec::modekeyr:
         if(!flocked) {
            errors.push_back("MODEKEYR write while unlocked");
            keySteps_.mode = 2;
         }
         if(keySequence(keySteps_.mode, value, "MODEKEYR")) {
            flocked = false;
         }
         break;
      case fpec::obkeyr:
         if(locked) {
            errors.push_back("OBKEYR write while locked");
         } else if(keySequence(keySteps_.option, value, "OBKEYR")) {
            obwre = true;
         }
         break;
      case fpec::statr:
         status &= ~(value & (fpec::eop | fpec::wrprterr));
         break;
      case fpec::ctlr:
         storeControl(value);
         break;
      default:
         break;
      }
      return true;
   }

   void storeControl(uint32_t value) {
      namespace fpec = wchlink::fpec;
      if(locked) {
         errors.push_back("CTLR write while locked");
         return;
      }
      locked = locked || (value & fpec::lock) != 0;
      flocked = flocked || (value & fpec::flock) != 0;
      obwre = obwre && (value & fpec::obwre) != 0;
      mode = value & opMask & ~fpec::strt;
      if((value & fpec::strt) == 0) {
         return;
      }
      if((mode & fpec::mer) != 0) {
         eraseMain();
         ++massErases;
      } else if((mode & fpec::ober) != 0 && obwre) {
         options.fill(0xffff);
         ++optionErases;
      } else {
         errors.push_back("STRT without an operation");
         return;
      }
      operationDone();
   }

   void storeOption(uint32_t address, uint32_t value, uint32_t bytes) {
      namespace fpec = wchlink::fpec;
      if(bytes != 2 || (mode & fpec::obpg) == 0 || !obwre || locked) {
         errors.push_back("option byte write without OBPG, or not a half word");
         return;
      }
      const std::size_t index = (address - fpec::optionBytes) / 2;
      if(index >= chip_.optionBytes) {
         errors.push_back("option byte that the chip does not have");
         return;
      }
      if(options[index] != 0xffff) {
         errors.push_back("option byte write without erase");
      }
      const auto byte = static_cast<uint8_t>(value);
      setOption(index, byte);
      if(index == fpec::optionRdpr && byte == fpec::rdprUnprotected && readProtected) {
         eraseMain();
         ++autoErases;
      }
      operationDone();
   }

   struct KeySteps {
      int key{ 0 };
      int mode{ 0 };
      int option{ 0 };
   };

   MockDebugModule &dm_;
   Chip chip_;
   KeySteps keySteps_;
   uint32_t busyLeft_{ 0 };
};

} // namespace vsltest
