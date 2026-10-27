#include "protocol/cmsisdap/RvBridge.h"

#include <array>

namespace cmsisdap {

namespace {

// The scan of jtagChain(): TMS high for 5 bits (Test-Logic-Reset), then
// Run-Test/Idle, Select-DR, Capture-DR, Shift-DR. Bits 9-72 are in Shift-DR,
// bit 72 goes to Exit1-DR, then Update-DR and Run-Test/Idle.
constexpr uint32_t chainScanBits = 75;
constexpr uint32_t chainFirstBit = 9;
constexpr uint32_t chainBits = 64;
constexpr std::array<uint8_t, 10> chainTms{ 0x5F, 0, 0, 0, 0, 0, 0, 0, 0, 0x03 };

// ADIv5 B5.2: a line reset (56 bits of SWDIO high), the JTAG-to-SWD
// sequence 0xE79E, a line reset, then 8 idle bits
constexpr std::array<uint8_t, 17> jtagToSwd{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x9E, 0xE7,
                                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
// A line reset, the SWD-to-JTAG sequence 0xE73C, then 8 bits with TMS high
// (Test-Logic-Reset)
constexpr std::array<uint8_t, 10> swdToJtag{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x3C, 0xE7, 0xFF };
// DAP_Transfer request: DP read of address 0 (DPIDR)
constexpr uint8_t readDpidr = dap::requestRead;

} // namespace

void RvBridge::setMode(BridgeMode mode) {
   mode_ = mode;
   // Auto keeps the port until the next init(): no pin activity on the
   // target, for example at the OpenOCD shutdown.
   const bool on = mode == BridgeMode::On || (mode == BridgeMode::Auto && on_);
   if(on == on_) {
      return;
   }
   if(active_) {
      port().fini();
   }
   on_ = on;
   if(active_) {
      port().init();
   }
}

bool RvBridge::storeMode(BridgeMode mode) {
   if(!store_.store(mode)) {
      return false;
   }
   stored_ = mode;
   return true;
}

bool RvBridge::detect() {
   if(jtagChain() || swdTarget()) {
      return false;
   }
   return tap_.probe();
}

bool RvBridge::jtagChain() {
   std::array<uint8_t, chainTms.size()> tdi{};
   tdi.fill(0xFF);
   std::array<uint8_t, chainTms.size()> tdo{};
   jtag_.init();
   const bool ok = jtag_.shift(chainScanBits, tdi, chainTms, tdo);
   jtag_.fini();
   if(!ok) {
      return false;
   }
   uint32_t ones = 0;
   for(uint32_t i = chainFirstBit; i < chainFirstBit + chainBits; ++i) {
      ones += (tdo[i / 8] >> (i % 8)) & 1u;
   }
   return ones != 0 && ones != chainBits;
}

bool RvBridge::swdTarget() {
   swd_.init();
   swd_.sequenceOut(static_cast<uint16_t>(jtagToSwd.size() * 8), jtagToSwd);
   uint32_t dpidr = 0;
   const bool found = swd_.transfer(readDpidr, dpidr) == dap::ackOk;
   if(found) {
      swd_.sequenceOut(static_cast<uint16_t>(swdToJtag.size() * 8), swdToJtag);
   }
   swd_.fini();
   return found;
}

} // namespace cmsisdap
