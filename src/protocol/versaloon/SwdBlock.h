#pragma once

// The USB_TO_SWD block (0x46): OpenOCD runs all SWD transfers through it.
// Hardware-independent: the block maps the requests to an SwdPort object,
// and the host unit tests use a fake one.
//
// Sub-commands (port index 0 only, values little-endian):
//
//   INIT    []                              -> [status]
//   FINI    []                              -> [status]
//   CONFIG  [trn u8][retry u16][delay u16]  -> [status]
//   OUT     [bitlen u16][data n]            -> [status]           (seqout)
//   IN      [bitlen u16]                    -> [status][data n]   (seqin)
//   IN_OUT  [request u8][data u32]          -> [status][ack u8][data u32]
//
// n = ceil(bitlen / 8); the first bit is the LSB of the first byte.
//
// INIT drives SWCLK and SWDIO and sets the default configuration. FINI
// releases them. CONFIG:
//
// - trn: the turnaround cycles of one transfer, both turnarounds together,
//   as in the original firmware. OpenOCD sends 2: one cycle at each
//   turnaround (the default of the target). 2, 4, 6 or 8; else
//   INVALID_PARA.
// - retry: the number of retries after a WAIT acknowledge.
// - delay: the SWCLK frequency, as the OpenOCD vsllink driver maps it (see
//   swd::clockHz() in swd/SwdBitBang.h).
//
// IN_OUT runs one transfer. Bits 1-4 of request are APnDP, RnW, A2 and A3;
// the firmware sets the start bit, the parity, the stop bit and the park
// bit. The reply data is the read data, or the request data for a write
// and for a failed read. The ack byte has the 3-bit acknowledge in bits
// 0-2 and these flags:
//
//   0x80  FAULT acknowledge
//   0x40  WAIT acknowledge after all retries
//   0x20  acknowledge that is not OK, WAIT or FAULT
//   0x10  parity error in the read data
//
// The status is OK only for an OK acknowledge without a parity error, else
// FAILED. OpenOCD does not read the ack byte, so the status is how it sees
// a failed transfer. (The original firmware replied OK in all cases.)
//
// A DP TARGETSEL write (multidrop SWD, DP address 0xC) is always OK. No
// target drives its acknowledge, so the ack byte is usually 0x27. The
// OpenOCD bitbang driver also ignores this acknowledge
// (swd_cmd_returns_ack()). The ack byte stays as read on the wire.
//
// errorCount() counts the IN_OUT transfers with the status FAILED, for the
// error LED (ErrorLed.h). The raw JTAG block has no acknowledge, so the
// Versaloon personality cannot see the errors of JTAG.

#include <cstdint>

#include "protocol/SwdPort.h"
#include "protocol/versaloon/UsbToXxx.h"

namespace protocol {

namespace swd {
// The clock setting after INIT: about 235 kHz
inline constexpr uint16_t defaultDelay = 1;

// The status of a transfer with this encoded request and ack byte
constexpr Status statusOf(uint8_t request, uint8_t ack) {
   return ack == ackOk || request == targetSelRequest ? Status::Ok : Status::Failed;
}
} // namespace swd

class SwdBlock final : public SubCommandBlock {
   public:
   explicit SwdBlock(SwdPort &port);

   // Number of IN_OUT transfers that failed. It wraps at 2^32: only the
   // difference between two values counts.
   [[nodiscard]] uint32_t errorCount() const {
      return errorCount_;
   }

   protected:
   void runSubCommand(const SubCommand &command, Reply &reply) override;

   private:
   [[nodiscard]] Status config(art::span<const uint8_t> payload);
   [[nodiscard]] Status sequenceOut(art::span<const uint8_t> payload);

   SwdPort &port_;
   uint32_t errorCount_{ 0 };
};

} // namespace protocol
