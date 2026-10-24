#include "protocol/wchlink/WchLinkFlash.h"

#include "protocol/wchlink/WchLinkProcessor.h"

namespace wchlink {

namespace {

namespace dm = rvswd::dm;

// dmcontrol: acknowledge havereset
inline constexpr uint32_t ackhavereset = 1u << 28;

// CSRs and GPRs of the stub call
inline constexpr uint16_t csrMstatus = 0x0300;
inline constexpr uint16_t csrDcsr = 0x07b0;
inline constexpr uint16_t gprSp = dm::gpr0 + 2;
inline constexpr uint16_t gprA2 = dm::gpr0 + 12;
// mstatus: machine interrupt enable
inline constexpr uint32_t mstatusMie = 1u << 3;
// dcsr: ebreak in machine mode enters debug mode
inline constexpr uint32_t dcsrEbreakm = 1u << 15;

inline constexpr uint32_t pollCount = Flash::operationTimeoutUs / Flash::pollIntervalUs;

// The padding of a block: erased flash
inline constexpr uint32_t erasedWord = 0xffff'ffffu;

constexpr uint32_t roundUp(uint32_t value, uint32_t multiple) {
   return (value + multiple - 1) / multiple * multiple;
}

bool ok(rvswd::Status status) {
   return status == rvswd::Status::Ok;
}

} // namespace

void optionsFromRegisters(Series series, uint32_t obr, uint32_t wpr, uint8_t (&values)[fpec::optionBytesV20x]) {
   if(series != Series::V20x) {
      values[1] = static_cast<uint8_t>(obr >> 2);
      values[2] = static_cast<uint8_t>(obr >> 10);
      values[3] = static_cast<uint8_t>(obr >> 18);
   } else {
      values[1] = static_cast<uint8_t>((obr >> 2 & 0x07) | 0x18 | (obr >> 8 & 0x07) << 5);
      values[2] = 0xff;
      values[3] = 0xff;
   }
   for(std::size_t i = 0; i < 4; ++i) {
      values[fpec::optionWrpr0 + i] = static_cast<uint8_t>(wpr >> (8 * i));
   }
}

Flash::Flash(Target &target, Platform &platform)
   : target_(target)
   , platform_(platform) {
}

bool Flash::read(uint32_t address, uint32_t &value) {
   return ok(target_.readMemory(address, value));
}

bool Flash::write(uint32_t address, uint32_t value) {
   return ok(target_.writeMemory(address, value, rvswd::Width::Word));
}

bool Flash::resetHalt() {
   if(!ok(target_.dmiWrite(dm::dmcontrol, dm::haltreq | dm::ndmreset | dm::dmactive))) {
      return false;
   }
   platform_.delayUs(resetPulseUs);
   return ok(target_.dmiWrite(dm::dmcontrol, dm::haltreq | dm::dmactive)) && ok(target_.halt()) &&
          ok(target_.dmiWrite(dm::dmcontrol, ackhavereset | dm::haltreq | dm::dmactive)) &&
          ok(target_.writeRegister(wchdm::csr7c0, wchdm::csr7c0Value));
}

bool Flash::unlockController() {
   uint32_t control = 0;
   if(!read(fpec::ctlr, control)) {
      return false;
   }
   // A key write to an unlocked controller locks it until the next reset.
   if((control & fpec::lock) != 0 && !(write(fpec::keyr, fpec::key1) && write(fpec::keyr, fpec::key2) && read(fpec::ctlr, control))) {
      return false;
   }
   return (control & fpec::lock) == 0;
}

bool Flash::waitIdle() {
   uint32_t status = fpec::bsy;
   for(uint32_t i = 0; (status & fpec::bsy) != 0; ++i) {
      if(i >= pollCount || !read(fpec::statr, status)) {
         return false;
      }
      if((status & fpec::bsy) != 0) {
         platform_.delayUs(pollIntervalUs);
      }
   }
   // Write 1 to clear EOP and WRPRTERR.
   return write(fpec::statr, fpec::eop | fpec::wrprterr) && (status & fpec::wrprterr) == 0;
}

bool Flash::lockController() {
   // A locked controller takes no CTLR write.
   uint32_t control = 0;
   return read(fpec::ctlr, control) && ((control & fpec::lock) != 0 || write(fpec::ctlr, control | fpec::lock | fpec::flock));
}

bool Flash::massErase() {
   if(!resetHalt() || !unlockController()) {
      return false;
   }
   const bool erased = write(fpec::ctlr, fpec::mer) && write(fpec::ctlr, fpec::mer | fpec::strt) && waitIdle();
   return write(fpec::ctlr, 0) && lockController() && erased;
}

bool Flash::setReadProtection(bool protect, Series series) {
   const std::size_t optionBytes = optionBytesOf(series);
   uint32_t obr = 0;
   uint32_t wpr = 0;
   if(!resetHalt() || !read(fpec::obr, obr) || !read(fpec::wpr, wpr)) {
      return false;
   }
   // The loaded values, then the bytes of the area that have their inverse
   uint8_t values[fpec::optionBytesV20x]{};
   optionsFromRegisters(series, obr, wpr, values);
   for(std::size_t i = 0; i < optionBytes; i += 2) {
      uint32_t word = 0;
      if(!read(fpec::optionBytes + 2 * i, word)) {
         // Hidden on a protected chip
         break;
      }
      for(std::size_t j = 0; j < 2; ++j) {
         const uint32_t half = word >> (16 * j);
         if(((half ^ half >> 8) & 0xff) == 0xff) {
            values[i + j] = static_cast<uint8_t>(half);
         }
      }
   }
   values[fpec::optionRdpr] = protect ? fpec::rdprProtected : fpec::rdprUnprotected;
   if(!protect) {
      for(std::size_t i = fpec::optionWrpr0; i < optionBytes; ++i) {
         values[i] = 0xff;
      }
   }

   if(!unlockController() || !write(fpec::obkeyr, fpec::key1) || !write(fpec::obkeyr, fpec::key2)) {
      return false;
   }
   uint32_t control = 0;
   if(!read(fpec::ctlr, control) || (control & fpec::obwre) == 0) {
      return false;
   }
   // OBWRE stays 1 in each write: a 0 locks the option bytes again.
   bool done =
   write(fpec::ctlr, fpec::ober | fpec::obwre) && write(fpec::ctlr, fpec::ober | fpec::strt | fpec::obwre) && waitIdle();
   // The controller writes the inverse byte itself. RDPR comes first: the chip
   // erases the main flash when it becomes unprotected.
   done = done && write(fpec::ctlr, fpec::obpg | fpec::obwre);
   for(std::size_t i = 0; i < optionBytes && done; ++i) {
      // The CH32V006 takes a half word only after STRT. FTPG, as the manual
      // says, does not set in FLASH_CTLR through the debug module (hardware).
      done = (series != Series::V006 || write(fpec::ctlr, fpec::obpg | fpec::strt | fpec::obwre)) &&
             ok(target_.writeMemory(fpec::optionBytes + 2 * i, values[i], rvswd::Width::Half)) && waitIdle();
   }
   done = write(fpec::ctlr, 0) && lockController() && done;
   // Unprotect: the reset loads FLASH_OBR from the new option bytes. The
   // manuals ask for a power-on reset in the unprotect procedure, but say
   // that a system reset loads FLASH_OBR. Not checked on hardware.
   return done && (protect || resetHalt());
}

bool Flash::beginStub(const StubLayout &layout) {
   active_ = false;
   unlocked_ = false;
   layout_ = &layout;
   stubReceived_ = 0;
   dataLength_ = 0;
   dataOffset_ = 0;
   if(!resetHalt() || !ok(target_.readRegister(dm::dpc, savedDpc_)) || !ok(target_.readRegister(csrDcsr, savedDcsr_)) ||
      !ok(target_.readRegister(csrMstatus, savedMstatus_))) {
      return false;
   }
   // The ebreak at the end of the stub halts the hart. No interrupt runs
   // code of the target during the stub.
   active_ = ok(target_.writeRegister(csrDcsr, savedDcsr_ | dcsrEbreakm)) &&
             ok(target_.writeRegister(csrMstatus, savedMstatus_ & ~mstatusMie));
   return active_;
}

bool Flash::stubData(art::span<const uint8_t> data) {
   for(std::size_t i = 0; i + 4 <= data.size() && stubReceived_ < stubSize; i += 4) {
      const uint32_t word = data[i] | data[i + 1] << 8 | data[i + 2] << 16 | static_cast<uint32_t>(data[i + 3]) << 24;
      if(!write(layout_->stubAddress + stubReceived_, word)) {
         return false;
      }
      stubReceived_ += 4;
   }
   return true;
}

bool Flash::runStub(uint32_t operation, uint32_t address, uint32_t length, uint32_t &result) {
   if(!active_ || !stubComplete() || !ok(target_.writeRegister(dm::a0, operation)) || !ok(target_.writeRegister(dm::a1, address)) ||
      !ok(target_.writeRegister(gprA2, length)) || !ok(target_.writeRegister(gprSp, layout_->stackTop)) ||
      !ok(target_.writeRegister(dm::dpc, layout_->stubAddress)) || !ok(target_.resume())) {
      return false;
   }
   bool halted = false;
   for(uint32_t i = 0; !halted; ++i) {
      if(i >= pollCount || !ok(target_.halted(halted))) {
         // The stub hangs: stop it.
         static_cast<void>(target_.halt());
         return false;
      }
      if(!halted) {
         platform_.delayUs(pollIntervalUs);
      }
   }
   return ok(target_.readRegister(dm::a0, result));
}

bool Flash::unlock() {
   // A second unlock would lock the controller until the next reset.
   uint32_t result = 0;
   unlocked_ = unlocked_ || (runStub(stubUnlock, 0, 0, result) && result == 0);
   return unlocked_;
}

bool Flash::beginData(uint32_t address, uint32_t length) {
   dataAddress_ = address;
   dataLength_ = length;
   dataOffset_ = 0;
   blockBytes_ = 0;
   checksum_ = 0;
   partWord_ = 0;
   partBytes_ = 0;
   blockDone_ = false;
   return active_ && unlocked_ && length != 0 && address % layout_->pageSize == 0;
}

bool Flash::bufferWord(uint32_t word) {
   checksum_ += word;
   const bool written = write(layout_->bufferAddress + blockBytes_, word);
   blockBytes_ += 4;
   return written;
}

bool Flash::data(art::span<const uint8_t> bytes) {
   blockDone_ = false;
   for(const uint8_t byte : bytes) {
      if(dataOffset_ >= dataLength_) {
         break;
      }
      partWord_ |= static_cast<uint32_t>(byte) << (8 * partBytes_);
      ++partBytes_;
      ++dataOffset_;
      if(partBytes_ == 4 && !bufferWord(partWord_)) {
         return false;
      }
      if(partBytes_ == 4) {
         partWord_ = 0;
         partBytes_ = 0;
      }
      if(blockBytes_ == layout_->blockSize || dataOffset_ == dataLength_) {
         if(!flushBlock()) {
            return false;
         }
         blockDone_ = true;
      }
   }
   return true;
}

bool Flash::flushBlock() {
   // The rest of the last word, then the rest of the page, as erased flash:
   // the stub programs whole pages.
   if(partBytes_ != 0) {
      const uint32_t word = partWord_ | erasedWord << (8 * partBytes_);
      partWord_ = 0;
      partBytes_ = 0;
      if(!bufferWord(word)) {
         return false;
      }
   }
   const uint32_t length = roundUp(blockBytes_, layout_->pageSize);
   while(blockBytes_ < length) {
      if(!bufferWord(erasedWord)) {
         return false;
      }
   }
   // The block holds the last byte taken.
   const uint32_t address = dataAddress_ + (dataOffset_ - 1) / layout_->blockSize * layout_->blockSize;
   uint32_t result = 0;
   const bool done = write(layout_->checksumAddress(), checksum_) &&
                     runStub(stubErasePages | stubProgram | stubVerify, address, length, result) && result == 0;
   blockBytes_ = 0;
   checksum_ = 0;
   return done;
}

bool Flash::end() {
   if(!active_) {
      return true;
   }
   active_ = false;
   return lockController() && ok(target_.writeRegister(dm::dpc, savedDpc_)) &&
          ok(target_.writeRegister(csrDcsr, savedDcsr_)) && ok(target_.writeRegister(csrMstatus, savedMstatus_));
}

} // namespace wchlink
