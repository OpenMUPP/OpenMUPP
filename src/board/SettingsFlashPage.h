#pragma once

// The flash page of the stored personality in an image with more than one
// personality (VSL_MULTI_PERSONALITY, personality/PersonalityStore.h). Target only.
//
// The page is the 1 KB at __vsl_settings_start: the linker script keeps it
// out of the FLASH region of the image (ldscripts/mem-f103.ld).
// stm32flash erases only the pages of the image, so a firmware update
// keeps the page. A mass erase erases it: the image then starts with its
// default personality.
//
// The flash controller stalls the CPU while it erases or writes: an erase
// takes up to 40 ms. Use the page only at the start or before a reset.

#include <cstddef>
#include <cstdint>

#include "personality/PersonalityStore.h"

namespace board {

class SettingsFlashPage final : public personality::FlashPage {
   public:
   static constexpr std::size_t sizeBytes = 1024;

   [[nodiscard]] art::span<const uint16_t> slots() const override;
   bool erase() override;
   bool program(std::size_t index, uint16_t value) override;
};

} // namespace board
