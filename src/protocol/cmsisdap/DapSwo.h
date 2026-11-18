// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The SWO receiver as the CMSIS-DAP command processor (DapProcessor.h) sees
// it: the receiver of the DAP_SWO_* commands of CMSIS-DAP v2.1, in UART
// (NRZ) mode. Hardware-independent. swo/SwoUart.h gives the implementation
// on the CH32V307. The processor keeps the transport, the mode and the baud
// rate of the commands.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace cmsisdap {

// The transports of DAP_SWO_Transport
namespace swoTransport {
inline constexpr uint8_t none = 0;
inline constexpr uint8_t dataCommand = 1;
inline constexpr uint8_t streaming = 2;
} // namespace swoTransport

// The modes of DAP_SWO_Mode
namespace swoMode {
inline constexpr uint8_t off = 0;
inline constexpr uint8_t uart = 1;
inline constexpr uint8_t manchester = 2;
} // namespace swoMode

// The control bits of DAP_SWO_Control, and the request bits of
// DAP_SWO_ExtendedStatus
namespace swoControl {
inline constexpr uint8_t start = 0x01;
} // namespace swoControl
namespace swoExtended {
inline constexpr uint8_t status = 0x01;
inline constexpr uint8_t count = 0x02;
} // namespace swoExtended

// The bits of the trace status (DAP_SWO_Status, _ExtendedStatus, _Data)
namespace swoStatus {
inline constexpr uint8_t active = 0x01;
inline constexpr uint8_t error = 0x40;
inline constexpr uint8_t overrun = 0x80;
} // namespace swoStatus

class DapSwo {
   public:
   struct Status {
      bool active;
      // An overflow of the buffer or a UART overrun
      bool overrun;
      // A framing or noise error
      bool framingError;
      // The bytes to read
      uint32_t count;
   };

   // The size of the trace buffer (DAP_Info 0xFD)
   [[nodiscard]] virtual uint32_t capacity() const = 0;
   // The baud rate that start() gives for a request. 0: out of range.
   [[nodiscard]] virtual uint32_t realBaud(uint32_t baud) const = 0;
   // Starts the capture with an empty buffer and no errors. Returns the
   // real baud rate. 0: out of range, and the receiver is off.
   virtual uint32_t start(uint32_t baud) = 0;
   // Stops the capture. The bytes in the buffer stay readable.
   virtual void stop() = 0;
   // True while the capture runs. Clears nothing.
   [[nodiscard]] virtual bool active() const = 0;
   // Takes up to data.size() received bytes and returns the count.
   [[nodiscard]] virtual std::size_t read(art::span<uint8_t> data) = 0;
   // The status. Clears the error flags: each flag tells of the errors
   // since the last status() or start().
   [[nodiscard]] virtual Status status() = 0;

   protected:
   ~DapSwo() = default;
};

} // namespace cmsisdap
