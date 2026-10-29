#include "board/SettingsFlashPage.h"

#include <cmsis_device.h>

#if defined(STM32F1)

// The start of the page: the reserved flash at the end (system-stm32 mem-F103x8.ld)
extern "C" const uint16_t __flash_reserved_start[];

namespace board {

namespace {

constexpr std::size_t slotCount = SettingsFlashPage::sizeBytes / sizeof(uint16_t);

// The HSI was on before unlock()
bool hsiWasOn = false;

// Unlocks the flash controller. The flash controller needs the HSI on to
// erase and write (RM0008 3.3.3). The system clock runs from the PLL, so
// the HSI can be off. finish() turns it off again.
void unlock() {
   hsiWasOn = (RCC->CR & RCC_CR_HSION) != 0;
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
   if(!hsiWasOn) {
      RCC->CR &= ~RCC_CR_HSION;
   }
   return ok;
}

} // namespace

art::span<const uint16_t> SettingsFlashPage::slots() const {
   return { __flash_reserved_start, slotCount };
}

bool SettingsFlashPage::erase() {
   unlock();
   FLASH->CR |= FLASH_CR_PER;
   FLASH->AR = reinterpret_cast<uint32_t>(__flash_reserved_start);
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
   auto *const slot = const_cast<volatile uint16_t *>(&__flash_reserved_start[index]);
   *slot = value;
   return finish(FLASH_CR_PG) && *slot == value;
}

} // namespace board

#elif defined(STM32H5) // RM0481 chapter 7

#include <array>
#include <peripherals/Rcc.h>

#include "personality/FlashWordSlot.h"

// The start of the sector: the reserved flash at the end (system-stm32 mem-H523xC.ld)
extern "C" const personality::FlashWord __flash_reserved_start[];

namespace board {

namespace {

using personality::FlashWord;

constexpr std::size_t slotCount = SettingsFlashPage::sizeBytes / personality::flashWordBytes;
constexpr uint32_t errorFlags = FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR;
constexpr uint32_t clearFlags =
FLASH_CCR_CLR_EOP | FLASH_CCR_CLR_WRPERR | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_STRBERR | FLASH_CCR_CLR_INCERR;

// The slots of the sector. slots() reads the sector into it.
std::array<uint16_t, slotCount> view{};

// Set while readWord() reads the sector, and by NMI_Handler() for a double
// ECC error in the sector.
volatile bool readingSector = false;
volatile bool eccError = false;

// Two banks: 2 x 256 KB on the xE parts, 2 x 128 KB on the xC parts. CMSIS
// reads the flash size from the flash size data register (RM0481 67.2).
uint32_t bankBytes() {
   return FLASH_BANK_SIZE;
}

uint32_t sectorOffset() {
   return reinterpret_cast<uint32_t>(__flash_reserved_start) - FLASH_BASE;
}

// The physical bank of the sector is bank 2. With SWAP_BANK, the address is in
// the other bank.
bool inPhysicalBank2() {
   const bool swapped = (FLASH->OPTSR_CUR & FLASH_OPTSR_SWAP_BANK) != 0;
   return (sectorOffset() >= bankBytes()) != swapped;
}

// The slot that FLASH_ECCCORR or FLASH_ECCDETR points to, slotCount when the
// address is not in the sector. ADDR_ECC counts flash words from the start
// of the bank (16 bits: 1 MB banks, RM0481 table 88).
std::size_t eccSlot(uint32_t eccRegister) {
   const uint32_t first = (sectorOffset() % bankBytes()) / personality::flashWordBytes;
   const uint32_t word = eccRegister & FLASH_ECCR_ADDR_ECC;
   const bool bank2 = (eccRegister & FLASH_ECCR_BK_ECC) != 0;
   if(bank2 != inPhysicalBank2() || word < first || word >= first + slotCount) {
      return slotCount;
   }
   return word - first;
}

// A single ECC correction (ECCC) gives no NMI and returns the corrected data,
// so a word with one written bit reads as erased. readWord() takes it as an
// ECC error, so the store never writes that word again.
void clearEccCorrection() {
   FLASH->ECCCORR = (FLASH->ECCCORR & FLASH_ECCR_ECCIE) | FLASH_ECCR_ECCC;
}

FlashWord readWord(std::size_t index) {
   const auto *const source = reinterpret_cast<const volatile uint32_t *>(&__flash_reserved_start[index]);
   clearEccCorrection();
   eccError = false;
   readingSector = true;
   const FlashWord word{ source[0], source[1], source[2], source[3] };
   __DSB();
   __ISB();
   // The NMI clears ECCD. Wait for it, so it comes before readingSector is
   // cleared: its delay after the read is not specified.
   while((FLASH->ECCDETR & FLASH_ECCR_ECCD) != 0) {
   }
   // Only a correction of this word: an instruction fetch can also set ECCC.
   // Limit: ECCCORR keeps only the first address, so a fetch correction just
   // before the read hides a correction of this word.
   const uint32_t ecccorr = FLASH->ECCCORR;
   if((ecccorr & FLASH_ECCR_ECCC) != 0) {
      eccError = eccError || eccSlot(ecccorr) == index;
      clearEccCorrection();
   }
   readingSector = false;
   return word;
}

void readSector() {
   for(std::size_t index = 0; index < slotCount; ++index) {
      const FlashWord word = readWord(index);
      view[index] = personality::slotOf(word, eccError);
   }
}

// Unlocks the flash controller. The ICACHE takes a cacheable write as an
// error, so it is off until finish(). Turning it off also invalidates it.
void unlock() {
   Rcc::disableICache();
   while((FLASH->NSSR & (FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE)) != 0) {
   }
   // Unlocking an unlocked register locks it until the next reset (RM0481 7.3.5)
   if((FLASH->NSCR & FLASH_CR_LOCK) != 0) {
      FLASH->NSKEYR = 0x45670123U;
      FLASH->NSKEYR = 0xCDEF89ABU;
   }
   FLASH->NSCCR = clearFlags;
}

// Waits for the end of the operation, locks the flash controller and turns
// the ICACHE on again. True when the operation had no error.
bool finish(uint32_t operation) {
   while((FLASH->NSSR & (FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE)) != 0) {
   }
   const bool ok = (FLASH->NSSR & errorFlags) == 0;
   FLASH->NSCR &= ~operation;
   FLASH->NSCCR = clearFlags;
   FLASH->NSCR |= FLASH_CR_LOCK;
   Rcc::enableICache();
   return ok;
}

} // namespace

art::span<const uint16_t> SettingsFlashPage::slots() const {
   readSector();
   return { view.data(), view.size() };
}

bool SettingsFlashPage::erase() {
   // BKSEL selects the physical bank
   const bool bank2 = inPhysicalBank2();
   const uint32_t sector = (sectorOffset() % bankBytes()) / FLASH_SECTOR_SIZE;
   unlock();
   FLASH->NSCR = (FLASH->NSCR & ~(FLASH_CR_SNB | FLASH_CR_BKSEL)) | FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos) |
                 (bank2 ? FLASH_CR_BKSEL : 0U);
   FLASH->NSCR |= FLASH_CR_START;
   if(!finish(FLASH_CR_SER | FLASH_CR_SNB | FLASH_CR_BKSEL)) {
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
   FLASH->NSCR |= FLASH_CR_PG;
   // The fourth 32-bit write fills the 128-bit write buffer and starts the write
   const FlashWord word = personality::flashWordOf(value);
   auto *const target = reinterpret_cast<volatile uint32_t *>(const_cast<FlashWord *>(&__flash_reserved_start[index]));
   for(std::size_t i = 0; i < word.size(); ++i) {
      target[i] = word[i];
   }
   __DSB();
   const bool ok = finish(FLASH_CR_PG);
   return ok && slots()[index] == value;
}

} // namespace board

// A double ECC error raises the NMI (RM0481 7.9.10). A write that a power
// loss cut can leave a flash word of the sector with a bad ECC: readWord()
// then gets a corrupt slot. Any other NMI stops here, as in system-stm32.
extern "C" void NMI_Handler() {
   const uint32_t eccdetr = FLASH->ECCDETR;
   if((eccdetr & FLASH_ECCR_ECCD) != 0 && (board::readingSector || board::eccSlot(eccdetr) < board::slotCount)) {
      FLASH->ECCDETR = FLASH_ECCR_ECCD;
      ICACHE->CR |= ICACHE_CR_CACHEINV;
      board::eccError = true;
      return;
   }
#if defined(DEBUG)
   if((DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0) {
      __BKPT(0);
   }
#endif
   for(;;) {
   }
}

#else
#error "SettingsFlashPage: no settings page for this STM32 family"
#endif
