#include "boot/Bootloader.h"

#include <cmsis_device.h>

#include <cstdint>

#include "boot/BootloaderEntry.h"

namespace {

// The ROM bootloader of the STM32H523xx and STM32H533xx (AN2606 Rev 70,
// Table 124): the initial MSP at +0, the reset vector at +4. The H503 has
// it at 0x0BF87000.
#if defined(STM32H523xx) || defined(STM32H533xx)
constexpr uint32_t romBootloaderAddress = 0x0BF97000;
#else
#error "Add the ROM bootloader address of this STM32H5 device (AN2606)"
#endif

// The TAMP registers need the RTC APB clock, a write to a backup register
// also needs PWR_DBPCR.DBP. Both are off after the reset.
void openBackupRegisters() {
   RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;
   static_cast<void>(RCC->APB3ENR); // the clock is on before the next access
   PWR->DBPCR |= PWR_DBPCR_DBP;
}

void closeBackupRegisters() {
   PWR->DBPCR &= ~PWR_DBPCR_DBP;
   RCC->APB3ENR &= ~RCC_APB3ENR_RTCAPBEN;
}

[[noreturn]] void jumpToRomBootloader() {
   const auto *vectors = reinterpret_cast<const volatile uint32_t *>(romBootloaderAddress);
   const uint32_t stack = vectors[0];
   const uint32_t entry = vectors[1];
   SCB->VTOR = romBootloaderAddress;
   // The ROM bootloader stack is below a stack limit of this firmware
   __set_MSPLIM(0);
   // The USB DFU of the ROM bootloader needs the interrupts (AN2606 section 4.1)
   __enable_irq();
   __DSB();
   __ISB();
   // In assembly: no stack access between the MSP change and the jump
   __asm volatile("msr msp, %0\n"
                  "bx %1\n"
                  :
                  : "r"(stack), "r"(entry)
                  : "memory");
   __builtin_unreachable();
}

} // namespace

[[noreturn]] void boot::enterBootloader() {
   openBackupRegisters();
   TAMP->BKP0R = bootRequest;
   // A system reset keeps the backup registers. It also puts the USB
   // peripheral and its internal D+ pull-up into the reset state, so the
   // host sees a disconnect.
   NVIC_SystemReset();
}

void boot::startRomBootloaderOnRequest() {
   openBackupRegisters();
   const bool requested = TAMP->BKP0R == bootRequest;
   if(requested) {
      // The next reset starts the firmware again
      TAMP->BKP0R = 0;
      // The write is posted: read it back before DBP and the clock go off
      static_cast<void>(TAMP->BKP0R);
   }
   closeBackupRegisters();
   if(requested) {
      jumpToRomBootloader();
   }
}
