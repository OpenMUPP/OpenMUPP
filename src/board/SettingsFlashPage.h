// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The settings page (personality/PersonalityStore.h): the stored personality
// of an image with more than one personality (MUPP_MULTI_PERSONALITY) and
// the stored mode of the RISC-V bridge (MUPP_CMSIS_DAP_RV_BRIDGE). Target only.
//
// STM32F1: the page is the 1 KB at __flash_reserved_start (the memory linker script of the image).
// STM32H5: the page is the 8 KB sector at __flash_reserved_start, the last
// sector of bank 2 (system-stm32 mem-H523xC.ld). Each log slot
// is one 128-bit flash word (personality/FlashWordSlot.h).
// CH32V30x: the page is the 256-byte fast erase page at __flash_reserved_start,
// the last page of the 480 KB flash (CMakeLists.txt). It is in the
// non-zero-wait area, behind the zero-wait area of the image, so a change of
// the memory split in the option bytes does not move the image over it. The
// erase code must be in the zero-wait area (RM 32.2.1): the image is. An erased
// half-word reads 0xE339, not 0xFFFF: the driver maps it (RM 32.5.7).
// The linker script keeps the page out of the FLASH region of the image.
// stm32flash erases only the pages of the image, so a firmware update
// keeps the page. A mass erase erases it: the image then starts with its
// default personality.
//
// The flash controller stalls the CPU while it erases or writes: an erase
// takes up to 40 ms on the STM32F1, a write about 50 us. Interrupts wait
// for that time, so the UART bridge can lose received bytes. On the STM32H5
// the page is in bank 2: code in bank 1 does not stall. On the CH32V30x,
// HCLK is halved for each erase and write (Flash.cpp of ch32v00x-system), so
// the UART bridge baud rate is wrong for up to 16 ms. The page is used at the start, before the reset of a
// personality switch, and for a stored bridge mode (ID_DAP_Vendor1 with
// bit 7). The log erases the page once in 512 stores (128 on the CH32V30x).

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
#elif defined(CH32V30x_D8C)
   static constexpr std::size_t sizeBytes = 256;
#else
#error "SettingsFlashPage: no settings page for this STM32 family"
#endif

   [[nodiscard]] art::span<const uint16_t> slots() const override;
   bool erase() override;
   bool program(std::size_t index, uint16_t value) override;
};

} // namespace board
