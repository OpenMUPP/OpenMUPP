// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The gate of the target pins and of the target power on a board with an
// adapter ID and a board ID (board/IdSense.h). The protocols ask it at each
// connect: DAP_Connect, the AVR programming mode and debug session, the
// WCH-Link attach and the Versaloon SWD and JTAG init. A protocol without a
// gate (nullptr) works as before. Hardware-independent: the host unit tests
// use a fake gate. board/BoardIdSense.h has the gate of the target.

#include <cstdint>

namespace protocol {

// The protocol of a connect, for the adapter check. None: no protocol (the
// target power).
enum class TargetProtocol : uint8_t { None, Swd, Jtag, RvSwd, Avr };

class TargetGate {
   public:
   // A connect: reads the adapter ID again, and logs a wrong adapter for
   // the protocol. False when the probe must not drive the target pins and
   // must not switch the target power on.
   virtual bool check(TargetProtocol protocol) = 0;
   // The result of the last check(), without a new reading
   [[nodiscard]] virtual bool isAllowed() const = 0;

   protected:
   ~TargetGate() = default;
};

} // namespace protocol
