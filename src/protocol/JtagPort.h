#pragma once

// The JTAG port: the interface of the JTAG engines (jtag/JtagBitBang.h,
// jtag/JtagFastPath.h). The JTAG_RAW block (protocol/JtagRawBlock.h) uses
// it.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace protocol {

// The JTAG signals of the debug connector.
class JtagPort {
   public:
   // Makes TCK, TMS and TDI outputs (TCK low) and TDO and RTCK inputs.
   virtual void init() = 0;
   // Makes TCK, TMS and TDI inputs.
   virtual void fini() = 0;
   // Sets the TCK frequency; 0 is adaptive clocking with RTCK. Returns
   // false when the port cannot do it (no change).
   [[nodiscard]] virtual bool setSpeed(uint32_t kHz) = 0;
   // Shifts bitCount bits: tdi, tms and tdo have byteCount(bitCount) bytes
   // each, and tdo is 0 on entry. Returns false on an RTCK timeout.
   [[nodiscard]] virtual bool
   shift(uint32_t bitCount, art::span<const uint8_t> tdi, art::span<const uint8_t> tms, art::span<uint8_t> tdo) = 0;

   protected:
   ~JtagPort() = default;
};

namespace jtagRaw {
// The bytes of a stream of bitCount bits
constexpr std::size_t byteCount(uint32_t bitCount) {
   return bitCount / 8 + (bitCount % 8 != 0 ? 1 : 0);
}
} // namespace jtagRaw

} // namespace protocol
