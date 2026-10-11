#include "board/SettingsFlashPage.h"

#include <cmsis_device.h>

// The start of the page (ldscripts/mem-f103*.ld)
extern "C" const uint16_t __vsl_settings_start[];

namespace board {

namespace {

constexpr std::size_t slotCount = SettingsFlashPage::sizeBytes / sizeof(uint16_t);

// Unlocks the flash controller. The flash controller needs the HSI on to
// erase and write (RM0008 3.3.3). The system clock runs from the PLL, so
// the HSI can be off.
void unlock() {
   RCC->CR |= RCC_CR_HSION;
   while((RCC->CR & RCC_CR_HSIRDY) == 0) {
   }
   if((FLASH->CR & FLASH_CR_LOCK) != 0) {
      FLASH->KEYR = FLASH_KEY1;
      FLASH->KEYR = FLASH_KEY2;
   }
   // Clear the flags of an earlier operation
   FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
}

// Waits for the end of the operation and locks the flash controller.
// True when the operation had no error.
bool finish(uint32_t operation) {
   while((FLASH->SR & FLASH_SR_BSY) != 0) {
   }
   const bool ok = (FLASH->SR & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) == 0;
   FLASH->CR &= ~operation;
   FLASH->SR = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
   FLASH->CR |= FLASH_CR_LOCK;
   return ok;
}

} // namespace

art::span<const uint16_t> SettingsFlashPage::slots() const {
   return { __vsl_settings_start, slotCount };
}

bool SettingsFlashPage::erase() {
   unlock();
   FLASH->CR |= FLASH_CR_PER;
   FLASH->AR = reinterpret_cast<uint32_t>(__vsl_settings_start);
   FLASH->CR |= FLASH_CR_STRT;
   if(!finish(FLASH_CR_PER)) {
      return false;
   }
   for(const uint16_t slot : slots()) {
      if(slot != personality::PersonalityStore::erasedSlot) {
         return false;
      }
   }
   return true;
}

bool SettingsFlashPage::program(std::size_t index, uint16_t value) {
   if(index >= slotCount) {
      return false;
   }
   unlock();
   FLASH->CR |= FLASH_CR_PG;
   // The flash takes a 16-bit write while PG is set.
   auto *const slot = const_cast<volatile uint16_t *>(&__vsl_settings_start[index]);
   *slot = value;
   return finish(FLASH_CR_PG) && *slot == value;
}

} // namespace board
