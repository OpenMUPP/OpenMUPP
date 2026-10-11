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

#include "protocol/JtagPort.h"
#include "protocol/UsbToXxx.h"

namespace protocol {

namespace jtagRaw {
// The TCK frequency after INIT
inline constexpr uint32_t defaultKHz = 1000;
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
