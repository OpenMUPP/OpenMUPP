#pragma once

// The USB_TO_JTAG_RAW block (0x47): OpenOCD shifts all JTAG bits through
// it. Hardware-independent: the block maps the requests to a JtagPort
// object, and the host unit tests use a fake one.
//
// Sub-commands (port index 0 only, values little-endian):
//
//   INIT    []                              -> [status]
//   FINI    []                              -> [status]
//   CONFIG  [kHz u32]                       -> [status]
//   IN_OUT  [bitlen u32][TDI n][TMS n]      -> [status][TDO n]
//
// n = ceil(bitlen / 8). Bit i of a stream is bit (i % 8) of byte i / 8:
// the first bit is the LSB of the first byte. The bits of the last byte
// above bitlen are 0 in the TDO reply.
//
// INIT drives TCK, TMS and TDI, and sets the speed to defaultKHz. FINI
// releases these pins. CONFIG sets the TCK frequency; kHz = 0 selects
// adaptive clocking with RTCK. A board without RTCK replies INVALID_PARA
// to kHz = 0. When RTCK does not follow TCK, IN_OUT stops and replies
// TIMEOUT. TRST and SRST are in the GPIO block (protocol/GpioBlock.h).

#include <cstddef>
#include <cstdint>

#include "protocol/UsbToXxx.h"

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
// The TCK frequency after INIT
inline constexpr uint32_t defaultKHz = 1000;

// The bytes of a stream of bitCount bits
constexpr std::size_t byteCount(uint32_t bitCount) {
   return bitCount / 8 + (bitCount % 8 != 0 ? 1 : 0);
}
} // namespace jtagRaw

class JtagRawBlock final : public SubCommandBlock {
   public:
   explicit JtagRawBlock(JtagPort &port);

   protected:
   void runSubCommand(const SubCommand &command, Reply &reply) override;

   private:
   JtagPort &port_;
};

} // namespace protocol
