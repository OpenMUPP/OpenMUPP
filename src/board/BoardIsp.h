// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The AVR ISP engine of board::current (avr/IspEngine.h). Target only.
//
// IspPins: the ISP pins for the engine. SCK is TCK, MOSI is TDI, MISO is
// TDO and RESET is SRST, with the polarity of DebugPins::srstActive. Each
// SCK, MOSI and MISO access is one store or load (hw::FastPin). now() is
// the cycle counter (board/CycleCounter.h).
//
// IspSpi: SPI2 in master mode 0, MSB first, polled. SCK on PB13, MISO on
// PB14, MOSI on PB15 (the pins of the JTAG fast path). The pins are GPIO
// between transfers, so the bit-bang path, JTAG and SWD can use them.
// CH32V307 only (BoardIspV30x.cpp).

#include <cmsis_device.h>
#include <cstdint>

#include "avr/IspEngine.h"
#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"

namespace board {

class IspPins {
   public:
   void init();
   void fini();
   void reset(bool asserted);

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
   }

   [[gnu::always_inline]] void sck(bool level) {
      Sck::write(level);
   }
   [[gnu::always_inline]] void mosi(bool level) {
      Mosi::write(level);
   }
   [[gnu::always_inline]] bool miso() {
      return Miso::read();
   }
   [[gnu::always_inline]] uint32_t now() {
      return cycleCount();
   }
   [[gnu::always_inline]] uint32_t coreHz() {
      return SystemCoreClock;
   }

   private:
   using Sck = FastPinOf<current.debug.tck>;
   using Mosi = FastPinOf<current.debug.tdi>;
   using Miso = FastPinOf<current.debug.tdo>;
};

class IspSpi {
   public:
   [[nodiscard]] uint32_t clockHz() const {
      return clockHz_;
   }
   void init();
   void fini();
   void setShift(uint8_t shift);
   [[nodiscard]] bool transfer(IspPins &pins, const uint8_t *out, uint8_t *in, uint32_t bytes);

   private:
   // SPI2 clock (APB1); init() reads it from the RCC.
   uint32_t clockHz_{ 72'000'000 };
   uint8_t shift_{ avr::spiMaxShift };
};

using BoardIsp = avr::IspEngine<IspPins, IspSpi>;

} // namespace board

// BoardIspV30x.cpp instantiates the engine, with -O2 (CMakeLists.txt).
#if defined(CH32V30x_D8C)
extern template class avr::IspEngine<board::IspPins, board::IspSpi>;
#endif
