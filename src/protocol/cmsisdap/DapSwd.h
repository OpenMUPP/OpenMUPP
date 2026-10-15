#pragma once

// The SWD engine as the CMSIS-DAP command processor (DapProcessor.h) sees
// it. Hardware-independent. BitBangDapSwd.h gives the adapter for the SWD
// bit-bang engine (swd/SwdBitBang.h).

#include <cstdint>
#include <utils/compat/span.h>

namespace cmsisdap {

// The values of DAP_TransferConfigure and DAP_SWD_Configure. The defaults
// are the values after a reset of the debug unit.
struct SwdSettings {
   // SWDIO low cycles after an OK transfer (0-255)
   uint8_t idleCycles{ 0 };
   // Cycles at each turnaround (1-4)
   uint8_t turnaround{ 1 };
   // Retries after a WAIT acknowledge
   uint16_t waitRetries{ 80 };
   // Run the data phase also after WAIT and FAULT
   bool dataPhase{ false };
};

namespace dap {
// The response value of a transfer (DAP_Transfer response byte)
inline constexpr uint8_t ackOk = 0x01;
inline constexpr uint8_t ackWait = 0x02;
inline constexpr uint8_t ackFault = 0x04;
inline constexpr uint8_t ackNone = 0x07;
// Parity error in the read data. This replaces the acknowledge.
inline constexpr uint8_t protocolError = 0x08;
inline constexpr uint8_t valueMismatch = 0x10;

// The request byte of a transfer
inline constexpr uint8_t requestApNotDp = 0x01;
inline constexpr uint8_t requestRead = 0x02;
inline constexpr uint8_t requestMatchValue = 0x10;
inline constexpr uint8_t requestMatchMask = 0x20;
// A2 and A3, and the DP register RDBUFF (read, address 0xC)
inline constexpr uint8_t requestAddressMask = 0x0C;
inline constexpr uint8_t readRdbuff = 0x0E;
} // namespace dap

// The DAP_TransferAbort state of the processor (DapProcessor.h)
class TransferAbort {
   public:
   // True: stop the transfers of the packet that runs
   [[nodiscard]] virtual bool aborted() const = 0;

   protected:
   ~TransferAbort() = default;
};

class DapSwd {
   public:
   // Drives SWCLK and SWDIO high.
   virtual void init() = 0;
   // Makes SWCLK and SWDIO inputs.
   virtual void fini() = 0;
   // SWCLK in Hz, rounded down to the nearest available rate.
   virtual void setClockHz(uint32_t hz) = 0;
   virtual void apply(const SwdSettings &settings) = 0;
   // Drives bitCount bits (1-256) on SWDIO, LSB first.
   virtual void sequenceOut(uint16_t bitCount, art::span<const uint8_t> data) = 0;
   // Releases SWDIO and reads bitCount bits into data (0 on entry).
   virtual void sequenceIn(uint16_t bitCount, art::span<uint8_t> data) = 0;
   // Runs one transfer. request: the DAP_Transfer request byte, bits 0-3
   // (APnDP, RnW, A2, A3). Writes data, or reads into data. Waits for the
   // WAIT retries of the settings. Returns the response value: the
   // acknowledge (bits 0-2), or protocolError for a parity error.
   virtual uint8_t transfer(uint8_t request, uint32_t &data) = 0;
   // After a WAIT, transfer() stops its retries when abort.aborted() is
   // true. It then returns the WAIT acknowledge. The processor calls it
   // once, at its construction. abort must stay valid.
   virtual void setAbort(const TransferAbort &abort) = 0;

   protected:
   ~DapSwd() = default;
};

} // namespace cmsisdap
