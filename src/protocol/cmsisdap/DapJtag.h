#pragma once

// The JTAG-DP of the CMSIS-DAP command processor (DapProcessor.h) on a JTAG
// port (protocol/JtagPort.h): the device chain of DAP_JTAG_Configure, the
// IR and DR scans, the DPACC and APACC transfers, ABORT and IDCODE.
// Hardware-independent: the host unit tests run it on the JTAG bit-bang
// engine with mock pins.
//
// transfer() gives a transfer the result that it has on SWD
// (DapSwd::transfer()). Thus the processor runs the same transfer rules on
// both ports:
//
// - An AP read is posted. It returns the data of the read before it.
// - A read of RDBUFF returns the data of the posted read.
// - A DP read returns its own data. The JTAG-DP posts each read, so
//   transfer() reads RDBUFF after a DP read.
//
// The device chain: device 0 is next to TDO. A scan puts the other devices
// in BYPASS. Each scan starts and ends in Run-Test/Idle, as in the
// reference implementation (CMSIS-DAP JTAG_DP.c). A DR scan ends with the
// idle cycles of DAP_TransferConfigure.
//
// The IR of the selected device is kept from one transfer to the next.
// invalidate() forgets it: after a sequence, a pin change or a change of
// the chain, the next transfer writes the IR again.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

#include "protocol/JtagPort.h"
#include "protocol/cmsisdap/DapSwd.h"

namespace cmsisdap {

// The instructions of the JTAG-DP
namespace jtagIr {
inline constexpr uint8_t abort = 0x08;
inline constexpr uint8_t dpacc = 0x0A;
inline constexpr uint8_t apacc = 0x0B;
inline constexpr uint8_t idcode = 0x0E;
} // namespace jtagIr

class JtagDp {
   public:
   // The devices of DAP_JTAG_Configure, and the IR length of one device
   static constexpr std::size_t maxDevices = 8;
   static constexpr uint8_t maxIrLength = 32;
   // The bits of the longest scan: an IR scan of maxDevices devices with
   // maxIrLength bits, or a DR scan of the last device with 255 idle
   // cycles.
   static constexpr std::size_t maxIrScanBits = 4 + maxDevices * maxIrLength + 2;
   static constexpr std::size_t maxDrScanBits = 3 + (maxDevices - 1) + 35 + 2 + 255;
   static constexpr std::size_t maxScanBits = std::max(maxIrScanBits, maxDrScanBits);
   // DAP_SWJ_Sequence has up to 256 bits.
   static_assert(maxScanBits >= 256);

   // port: nullptr for a board without JTAG.
   explicit JtagDp(protocol::JtagPort *port)
      : port_(port) {
   }

   [[nodiscard]] bool available() const {
      return port_ != nullptr;
   }

   // Needs available().
   [[nodiscard]] protocol::JtagPort &port() {
      return *port_;
   }

   // DAP_JTAG_Configure: the IR length of each device, device 0 first.
   // More than maxDevices devices, or an IR length of 0 or more than
   // maxIrLength, gives false and an empty chain.
   [[nodiscard]] bool configure(art::span<const uint8_t> irLengths);

   [[nodiscard]] std::size_t deviceCount() const {
      return deviceCount_;
   }

   // The device of the next scans. False when the chain has no such device.
   [[nodiscard]] bool select(uint8_t index);

   // The idle cycles and the WAIT retries
   void apply(const SwdSettings &settings) {
      idleCycles_ = settings.idleCycles;
      waitRetries_ = settings.waitRetries;
   }

   void invalidate() {
      ir_ = noIr;
   }

   // One transfer with the selected device, as DapSwd::transfer(). Retries
   // after WAIT. Returns the acknowledge: dap::ackOk or dap::ackWait, or
   // another value for a bad acknowledge or an RTCK timeout.
   [[nodiscard]] uint8_t transfer(uint8_t request, uint32_t &data);

   // Writes ABORT of the selected device.
   void writeAbort(uint32_t data);

   // Reads IDCODE of the selected device. False on an RTCK timeout.
   [[nodiscard]] bool readIdcode(uint32_t &value);

   // DAP_JTAG_Sequence: bitCount bits (1-64) with one TMS level. tdo gets
   // the TDO bits when it is not empty.
   [[nodiscard]] bool sequence(uint8_t bitCount, bool tms, art::span<const uint8_t> tdi, art::span<uint8_t> tdo);

   // DAP_SWJ_Sequence: bitCount bits (1-256) on TMS, TDI high.
   [[nodiscard]] bool tmsSequence(uint16_t bitCount, art::span<const uint8_t> data);

   private:
   static constexpr uint8_t noIr = 0xFF;

   // Writes the IR of the selected device. The other devices get BYPASS.
   [[nodiscard]] bool setIr(uint8_t instruction);
   // One DR scan of a DPACC, APACC or ABORT request (3 bits of request, 32
   // bits of data). Returns the acknowledge in the CMSIS-DAP encoding.
   [[nodiscard]] uint8_t scan(uint8_t request, uint32_t &data);
   [[nodiscard]] uint8_t scanWithRetries(uint8_t request, uint32_t &data);

   // The scan buffers: start(), add bits, run(), then read the TDO bits.
   void start();
   void put(uint32_t value, uint32_t count, bool tms);
   void putOnes(uint32_t count, bool tms);
   [[nodiscard]] bool run();
   [[nodiscard]] uint32_t tdoBits(uint32_t first, uint32_t count) const;

   protocol::JtagPort *port_;
   std::array<uint8_t, maxDevices> irLength_{};
   std::size_t deviceCount_{ 0 };
   uint8_t index_{ 0 };
   uint8_t ir_{ noIr };
   uint8_t idleCycles_{ 0 };
   uint16_t waitRetries_{ 0 };

   static constexpr std::size_t scanBytes = (maxScanBits + 7) / 8;
   std::array<uint8_t, scanBytes> tdi_{};
   std::array<uint8_t, scanBytes> tms_{};
   std::array<uint8_t, scanBytes> tdo_{};
   uint32_t bits_{ 0 };
};

} // namespace cmsisdap
