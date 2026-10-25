#pragma once

// The SWD port: the interface of the SWD engine (swd/SwdBitBang.h). The
// Versaloon SWD block (protocol/versaloon/SwdBlock.h) and the CMSIS-DAP
// SWD transfers (protocol/cmsisdap/BitBangDapSwd.h) use it. The ack byte
// of transfer() has the format of the Versaloon SWD block.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace protocol {

namespace swd {
// The acknowledge of a transfer (bits 0-2 of the ack byte)
inline constexpr uint8_t ackOk = 0x01;
inline constexpr uint8_t ackWait = 0x02;
inline constexpr uint8_t ackFault = 0x04;
inline constexpr uint8_t ackMask = 0x07;

// The flags of the ack byte
inline constexpr uint8_t faultFlag = 0x80;
inline constexpr uint8_t retryOutFlag = 0x40;
inline constexpr uint8_t ackErrorFlag = 0x20;
inline constexpr uint8_t parityErrorFlag = 0x10;

// RnW bit of a request
inline constexpr uint8_t readRequest = 0x04;

// The configuration of a new port: one turnaround cycle, no retry. INIT of
// the Versaloon SWD block sets it again.
inline constexpr uint8_t defaultTurnaround = 1;
inline constexpr uint16_t defaultRetries = 0;

// The parity of a word: 1 for an odd number of 1 bits. Always inline: the
// SWD engine (swd/SwdBitBang.h) uses it between the data bits and the
// parity bit, and LTO otherwise makes it a call.
[[gnu::always_inline]] constexpr bool parity(uint32_t value) {
   value ^= value >> 16;
   value ^= value >> 8;
   value ^= value >> 4;
   value ^= value >> 2;
   value ^= value >> 1;
   return (value & 1u) != 0;
}

// The 8-bit request of the wire from APnDP, RnW, A2 and A3 (bits 1-4):
// start bit 1 (bit 0), parity of bits 1-4 (bit 5), stop bit 0 (bit 6),
// park bit 1 (bit 7).
constexpr uint8_t encodeRequest(uint8_t request) {
   const uint8_t fields = request & 0x1E;
   return static_cast<uint8_t>(0x81 | fields | (parity(fields) ? 0x20 : 0));
}

// The encoded DP TARGETSEL write: APnDP 0, RnW 0, A2 1, A3 1
inline constexpr uint8_t targetSelRequest = 0x99;

// The bytes of a sequence of bitCount bits
constexpr std::size_t byteCount(uint16_t bitCount) {
   return (bitCount + 7u) / 8u;
}
} // namespace swd

// The SWD signals of the debug connector.
class SwdPort {
   public:
   // Drives SWCLK (high) and SWDIO (high).
   virtual void init() = 0;
   // Makes SWCLK and SWDIO inputs.
   virtual void fini() = 0;
   // turnaround: cycles at each turnaround, 1-4. retries: after WAIT.
   // delay: the clock setting of OpenOCD.
   virtual void configure(uint8_t turnaround, uint16_t retries, uint16_t delay) = 0;
   // Drives bitCount bits of data (byteCount(bitCount) bytes) on SWDIO.
   virtual void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) = 0;
   // Releases SWDIO and reads bitCount bits into data (0 on entry).
   virtual void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) = 0;
   // Runs one transfer with an encoded request (encodeRequest()). Writes
   // data, or reads into data. Returns the ack byte.
   virtual uint8_t transfer(uint8_t request, uint32_t &data) = 0;

   protected:
   ~SwdPort() = default;
};

} // namespace protocol
