#pragma once

// The flash page of the stored personality in an image with more than one
// personality (VSL_MULTI_PERSONALITY, personality/PersonalityStore.h). Target only.
//
// STM32F1: the page is the 1 KB at __vsl_settings_start (ldscripts/mem-f103.ld).
// STM32H5: the page is the 8 KB sector at __vsl_settings_start, the last
// sector of bank 2 (ldscripts/mem-h533.ld). Each log slot is one 128-bit
// flash word (personality/FlashWordSlot.h).
// The linker script keeps the page out of the FLASH region of the image.
// stm32flash erases only the pages of the image, so a firmware update
// keeps the page. A mass erase erases it: the image then starts with its
// default personality.
//
// The flash controller stalls the CPU while it erases or writes: an erase
// takes up to 40 ms on the STM32F1. Use the page only at the start or
// before a reset.

#include <cstddef>
#include <cstdint>

#include "personality/PersonalityStore.h"

namespace board {

class SettingsFlashPage final : public personality::FlashPage {
   public:
#if defined(STM32F1)
   static constexpr std::size_t sizeBytes = 1024;
#elif defined(STM32H5)
   static constexpr std::size_t sizeBytes = 8 * 1024;
#else
#error "SettingsFlashPage: no settings page for this STM32 family"
#endif

   [[nodiscard]] art::span<const uint16_t> slots() const override;
   bool erase() override;
   bool program(std::size_t index, uint16_t value) override;
};

} // namespace board
