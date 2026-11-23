// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The debug log on UART5 of the CH32V307 (board::current.debugUart: TX on
// PC12, RX on PD2 on OpenMUPP v1), 115200 baud 8N1. Target only. The port is
// the Port of debug::LogSink::drainTo() (debug/LogSink.h): DMA2 channel 4
// sends a block that the port copies out of the log sink, so the main loop
// never waits for the UART. The port has no receiver: PD2 is only an input
// with a pull-up.

#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace debug {

class DebugUart {
   public:
   // Turns on the clocks, sets the pins and starts the UART. Call it once.
   void start();

   // The log has no terminal state: the UART is always open.
   [[nodiscard]] bool isTerminalOpen() const {
      return true;
   }

   // The room of the block buffer: all of it when the DMA is idle, else 0.
   [[nodiscard]] std::size_t writeAvailable() const;

   // Copies up to writeAvailable() bytes to the block buffer and starts the
   // DMA. Returns the number of bytes taken.
   std::size_t write(art::span<const uint8_t> data);

   private:
   static constexpr std::size_t blockSize = 64;
   std::array<uint8_t, blockSize> block_{};
};

} // namespace debug
