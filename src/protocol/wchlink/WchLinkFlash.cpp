#include "protocol/wchlink/WchLinkFlash.h"

#include "protocol/wchlink/WchLinkProcessor.h"

namespace wchlink {

namespace {

namespace dm = rvswd::dm;

// dmcontrol: acknowledge havereset
inline constexpr uint32_t ackhavereset = 1u << 28;

inline constexpr uint32_t pollCount = Flash::operationTimeoutUs / Flash::pollIntervalUs;

bool ok(rvswd::Status status) {
   return status == rvswd::Status::Ok;
}

} // namespace

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

bool Flash::setReadProtection(bool protect, std::size_t optionBytes) {
   if(!resetHalt()) {
      return false;
   }
   // The old values. A byte with a wrong inverse reads as erased.
   uint8_t values[fpec::optionBytesV20x]{};
   if(optionBytes > fpec::optionBytesV20x) {
      return false;
   }
   for(std::size_t i = 0; i < optionBytes; i += 2) {
      uint32_t word = 0;
      if(!read(fpec::optionBytes + 2 * i, word)) {
         return false;
      }
      for(std::size_t j = 0; j < 2; ++j) {
         const uint32_t half = word >> (16 * j);
         values[i + j] = ((half ^ half >> 8) & 0xff) == 0xff ? static_cast<uint8_t>(half) : 0xff;
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
   bool done = write(fpec::ctlr, fpec::ober | fpec::obwre) && write(fpec::ctlr, fpec::ober | fpec::strt | fpec::obwre) &&
               waitIdle() && write(fpec::ctlr, fpec::obpg | fpec::obwre);
   // The controller writes the inverse byte itself. RDPR comes first: the
   // chip erases the main flash when it becomes unprotected.
   for(std::size_t i = 0; i < optionBytes && done; ++i) {
      done = ok(target_.writeMemory(fpec::optionBytes + 2 * i, values[i], rvswd::Width::Half)) && waitIdle();
   }
   done = write(fpec::ctlr, 0) && lockController() && done;
   // Unprotect: the reset loads FLASH_OBR from the new option bytes.
   return done && (protect || resetHalt());
}

} // namespace wchlink
