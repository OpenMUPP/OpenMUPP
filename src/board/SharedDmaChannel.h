// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// A DMA channel with two users that must not run at the same time. On the
// CH32V307, DMA2 channel 2 is the TMS channel of the JTAG fast path
// (board/BoardJtagFastV30x.cpp) and the falling-edge channel of the SWO
// Manchester capture (swo/SwoReceiver.h). Each processor and engine that
// drives the JTAG port (the CMSIS-DAP bulk and HID processors, the RISC-V
// bridge, the AVR JTAG target) goes through JtagSpi::init(), so the
// exclusion is here and not in one processor.
//
// - JTAG has priority: takeForJtag() stops the capture (its release
//   function) before the fast path sets up the channel.
// - The capture gets the channel only while JTAG does not have it, that is
//   between JtagSpi::fini() and the next JtagSpi::init().
//
// No CMSIS dependency, so the host unit tests can check it. Main loop
// context only: no lock.

namespace board {

class SharedDmaChannel {
   public:
   using Release = void (*)();

   // The JTAG fast path takes the channel. A running capture stops first.
   void takeForJtag() {
      const Release release = release_;
      release_ = nullptr;
      if(release != nullptr) {
         release();
      }
      jtag_ = true;
   }
   void releaseFromJtag() {
      jtag_ = false;
   }

   // The capture takes the channel. False while JTAG has it. release stops
   // the capture when JTAG takes the channel.
   [[nodiscard]] bool takeForCapture(Release release) {
      if(jtag_) {
         return false;
      }
      release_ = release;
      return true;
   }
   void releaseFromCapture() {
      release_ = nullptr;
   }

   [[nodiscard]] bool jtagHasIt() const {
      return jtag_;
   }

   private:
   Release release_ = nullptr;
   bool jtag_ = false;
};

// DMA2 channel 2 of the CH32V307
inline SharedDmaChannel dma2Channel2;

} // namespace board
