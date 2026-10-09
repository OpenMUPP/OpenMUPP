#pragma once

// The JTAG port of board::current. Target only. With the JTAG fast path
// (board::Definition::jtagFastPath), it is the SPI and DMA port
// (jtag/JtagFastPath.h), else the bit-bang port (jtag/JtagBitBang.h).
//
// JtagPins: the JTAG pins for the bit-bang engine. Each pin access is one
// store to BSRR or one load from IDR (hw::FastPin). now() is the DWT cycle
// counter.
//
// JtagSpi: the SPI and DMA of the fast path (BoardJtagFast.cpp). Its pins
// are fixed (see board::isValid()):
//
//   SPI2 master: SCK = TCK (PB13), MISO = TDO (PB14), MOSI = TDI (PB15)
//   SPI1 slave, remapped: SCK = TCK link (PB3), MISO = TMS (PB4)
//   DMA1: channel 3 = SPI1 TX (TMS), 4 = SPI2 RX (TDO), 5 = SPI2 TX (TDI)
//
// SPI1 MOSI (PB5) and NSS (PA15) stay GPIO: SPI1 ignores its receive data,
// and its NSS is software. The pins are GPIO between transfers, so the
// bit-bang engine and the SWD engine can use them.

#include <cmsis_device.h>
#include <cstdint>
#include <type_traits>

#include "board/Board.h"
#include "board/BoardPins.h"
#include "board/CycleCounter.h"
#include "jtag/JtagBitBang.h"
#include "jtag/JtagFastPath.h"

namespace board {

class JtagPins {
   public:
   static constexpr bool hasRtck = current.debug.rtck.has_value();

   void init(bool tckLevel);
   void fini();

   [[gnu::always_inline]] void begin() {
      startCycleCounter();
   }

   [[gnu::always_inline]] void tck(bool level) {
      Tck::write(level);
   }
   [[gnu::always_inline]] void tms(bool level) {
      Tms::write(level);
   }
   [[gnu::always_inline]] void tdi(bool level) {
      Tdi::write(level);
   }
   [[gnu::always_inline]] bool tdo() {
      return Tdo::read();
   }
   [[gnu::always_inline]] bool rtck() {
      return Rtck::read();
   }
   [[gnu::always_inline]] uint32_t now() {
      return cycleCount();
   }
   [[gnu::always_inline]] uint32_t coreHz() {
      return SystemCoreClock;
   }

   private:
   using Tck = FastPinOf<current.debug.tck>;
   using Tms = FastPinOf<current.debug.tmsOut>;
   using Tdi = FastPinOf<current.debug.tdi>;
   using Tdo = FastPinOf<current.debug.tdo>;
   // Without RTCK, the engine never reads it (hasRtck is false).
   using Rtck = FastPinOf<current.debug.rtck.value_or(current.debug.tdo)>;
};

class JtagSpi {
   public:
   [[nodiscard]] uint32_t clockHz() const {
      return clockHz_;
   }
   void init();
   void fini();
   void setShift(uint8_t shift);
   [[nodiscard]] bool transfer(JtagPins &pins, const uint8_t *tdi, const uint8_t *tms, uint8_t *tdo, uint32_t bytes);

   private:
   // SPI2 clock (APB1); init() reads it from the RCC.
   uint32_t clockHz_{ 36'000'000 };
   uint8_t shift_{ jtag::spiMinShift };
};

using BoardJtag = std::conditional_t<current.hasJtagFastPath(), jtag::FastPort<JtagPins, JtagSpi>, jtag::BitBangPort<JtagPins>>;

} // namespace board

// BoardJtag.cpp instantiates both engines, with -O2 (CMakeLists.txt). The
// linker drops the one that the board does not use. The fast path uses
// bit-bang with TCK idle high.
extern template class jtag::BitBangPort<board::JtagPins>;
extern template class jtag::BitBangPort<board::JtagPins, jtag::TckIdle::High>;
extern template class jtag::FastPort<board::JtagPins, board::JtagSpi>;
