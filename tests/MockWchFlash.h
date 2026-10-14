#pragma once

// A mock flash controller (FPEC) of a CH32V00x or CH32V20x, and a model of
// the host stub of the program sequence, on the hooks of a MockDebugModule.
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
// - The stub: when the hart resumes at the stub address, the model checks
//   the stub bytes in RAM and the call registers, does the operations of
//   a0 (the calling convention of the captured stubs), puts the result in
//   a0 and halts the hart, as the ebreak of the stub does.
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
      wchlink::StubLayout layout;
   };
   // The flash sizes are small to keep the tests fast.
   static constexpr Chip v003{ 16 * 1024, 64, 0xffff'ffff, wchlink::fpec::optionBytesV00x, wchlink::layoutV00x };
   static constexpr Chip v208{ 32 * 1024, 256, 0xe339'e339, wchlink::fpec::optionBytesV20x, wchlink::layoutV20x };

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
      dm_.onResume = [this] { resume(); };
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

   // The stub that the host sent. The hart runs garbage when RAM holds
   // other bytes.
   std::vector<uint8_t> stub;
   // The stub does not reach its ebreak.
   bool stubHangs{ false };
   // The next stub program writes this word wrong.
   bool corruptNextProgram{ false };
   struct Call {
      uint32_t operation;
      uint32_t address;
      uint32_t length;
   };
   std::vector<Call> calls;

   int massErases{ 0 };
   int autoErases{ 0 };
   int optionErases{ 0 };
   std::vector<std::string> errors;

   private:
   static constexpr uint32_t opMask = 0x00ff'0000u | 0x3fu;
   static constexpr uint16_t csrDcsr = 0x07b0;
   static constexpr uint16_t csrMstatus = 0x0300;
   // An ebreak of the stub
   static constexpr uint32_t stubEbreak = 0x15c;

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

   // The stub runs.
   void resume() {
      if(dm_.csrs[rvswd::dm::dpc] != chip_.layout.stubAddress) {
         return;
      }
      for(std::size_t i = 0; i < wchlink::stubSize; ++i) {
         const uint8_t expected = i < stub.size() ? stub[i] : 0;
         if(dm_.load(chip_.layout.stubAddress + i, 1) != expected) {
            errors.push_back("the stub in RAM is not the stub of the host");
            return;
         }
      }
      if((dm_.csrs[csrDcsr] & 1u << 15) == 0) {
         errors.push_back("ebreakm is 0: the ebreak of the stub does not halt");
         return;
      }
      if((dm_.csrs[csrMstatus] & 1u << 3) != 0) {
         errors.push_back("interrupts are on while the stub runs");
      }
      if(dm_.gprs[2] != chip_.layout.stackTop) {
         errors.push_back("wrong stack pointer");
      }
      if(stubHangs) {
         return;
      }
      const uint32_t operation = dm_.gprs[10];
      const uint32_t address = dm_.gprs[11];
      const uint32_t length = dm_.gprs[12];
      calls.push_back({ operation, address, length });
      dm_.gprs[10] = runStub(operation, address, length);
      dm_.csrs[rvswd::dm::dpc] = chip_.layout.stubAddress + stubEbreak;
      dm_.halted = true;
   }

   uint32_t runStub(uint32_t operation, uint32_t address, uint32_t length) {
      namespace fpec = wchlink::fpec;
      const uint32_t page = chip_.pageSize;
      const uint32_t pages = (length + page - 1) / page;
      if((operation & wchlink::stubUnlock) != 0) {
         store(fpec::keyr, fpec::key1, 4);
         store(fpec::keyr, fpec::key2, 4);
         store(fpec::modekeyr, fpec::key1, 4);
         store(fpec::modekeyr, fpec::key2, 4);
      }
      if((operation & (wchlink::stubErasePages | wchlink::stubProgram)) != 0 &&
         (address % page != 0 || !inFlash(address) || !inFlash(address + pages * page - 1))) {
         errors.push_back("stub pages outside the flash or not aligned");
         return 0x08;
      }
      const bool open = !locked && !flocked;
      if((operation & wchlink::stubErasePages) != 0 && open) {
         for(uint32_t i = 0; i < pages * page; i += 4) {
            setWord(address + i, chip_.erasedWord);
         }
      }
      if((operation & wchlink::stubProgram) != 0 && open) {
         for(uint32_t i = 0; i < pages * page; i += 4) {
            if(word(address + i) != chip_.erasedWord) {
               errors.push_back("stub program of flash that is not erased");
            }
            uint32_t value = dm_.load(chip_.layout.bufferAddress + i, 4);
            if(corruptNextProgram) {
               corruptNextProgram = false;
               value ^= 1;
            }
            setWord(address + i, value);
         }
      }
      if((operation & wchlink::stubVerify) != 0) {
         uint32_t sum = 0;
         for(uint32_t i = 0; i < (length + 3) / 4; ++i) {
            const uint32_t value = word(address + 4 * i);
            if(value != dm_.load(chip_.layout.bufferAddress + 4 * i, 4)) {
               return 0x10;
            }
            sum += value;
         }
         if(sum != dm_.load(chip_.layout.checksumAddress(), 4)) {
            return 0x10;
         }
      }
      return 0;
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
