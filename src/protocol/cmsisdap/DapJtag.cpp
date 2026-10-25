#include "protocol/cmsisdap/DapJtag.h"

#include <algorithm>

namespace cmsisdap {

bool JtagDp::configure(art::span<const uint8_t> irLengths) {
   invalidate();
   deviceCount_ = 0;
   if(irLengths.size() > maxDevices) {
      return false;
   }
   for(const uint8_t length : irLengths) {
      if(length == 0 || length > maxIrLength) {
         return false;
      }
   }
   std::copy(irLengths.begin(), irLengths.end(), irLength_.begin());
   deviceCount_ = irLengths.size();
   return true;
}

bool JtagDp::select(uint8_t index) {
   if(index >= deviceCount_) {
      return false;
   }
   if(index != index_) {
      // The IR scan of another device puts other devices in BYPASS.
      invalidate();
      index_ = index;
   }
   return true;
}

// put() and run() clear only the bytes of the scan: a clear of all buffers
// for each sequence took as long as the DMI frame of the RISC-V bridge
// (#114).
void JtagDp::start() {
   bits_ = 0;
}

// Adds count bits of value, LSB first. The bits above bit 31 are 0.
void JtagDp::put(uint32_t value, uint32_t count, bool tms) {
   for(uint32_t i = 0; i < count && bits_ < maxScanBits; ++i) {
      const std::size_t byte = bits_ / 8;
      const auto mask = static_cast<uint8_t>(1u << (bits_ % 8));
      if(mask == 1u) {
         tdi_[byte] = 0;
         tms_[byte] = 0;
      }
      if(i < 32 && ((value >> i) & 1u) != 0) {
         tdi_[byte] = static_cast<uint8_t>(tdi_[byte] | mask);
      }
      if(tms) {
         tms_[byte] = static_cast<uint8_t>(tms_[byte] | mask);
      }
      ++bits_;
   }
}

// Adds count bits with TDI high: BYPASS and the TAP state moves.
void JtagDp::putOnes(uint32_t count, bool tms) {
   while(count != 0) {
      const uint32_t bits = std::min<uint32_t>(count, 32);
      put(0xFFFFFFFFu, bits, tms);
      count -= bits;
   }
}

bool JtagDp::run() {
   const std::size_t bytes = protocol::jtagRaw::byteCount(bits_);
   std::fill_n(tdo_.begin(), bytes, 0);
   return port_->shift(bits_, { tdi_.data(), bytes }, { tms_.data(), bytes }, { tdo_.data(), bytes });
}

uint32_t JtagDp::tdoBits(uint32_t first, uint32_t count) const {
   uint32_t value = 0;
   for(uint32_t i = 0; i < count; ++i) {
      const uint32_t bit = first + i;
      if(((tdo_[bit / 8] >> (bit % 8)) & 1u) != 0) {
         value |= 1u << i;
      }
   }
   return value;
}

bool JtagDp::setIr(uint8_t instruction) {
   if(index_ >= deviceCount_) {
      return false;
   }
   if(ir_ == instruction) {
      return true;
   }
   uint32_t before = 0;
   uint32_t after = 0;
   for(std::size_t i = 0; i < deviceCount_; ++i) {
      if(i < index_) {
         before += irLength_[i];
      } else if(i > index_) {
         after += irLength_[i];
      }
   }
   const uint32_t length = irLength_[index_];
   start();
   // Select-DR-Scan, Select-IR-Scan, Capture-IR, Shift-IR
   putOnes(2, true);
   putOnes(2, false);
   // BYPASS (all ones) for the devices before and after
   putOnes(before, false);
   put(instruction, length - 1, false);
   // The last bit of the scan goes to Exit1-IR.
   put(instruction >> (length - 1), 1, after == 0);
   if(after != 0) {
      putOnes(after - 1, false);
      putOnes(1, true);
   }
   // Update-IR, Run-Test/Idle
   putOnes(1, true);
   putOnes(1, false);
   if(!run()) {
      invalidate();
      return false;
   }
   ir_ = instruction;
   return true;
}

uint8_t JtagDp::scan(uint8_t request, uint32_t &data) {
   const bool read = (request & dap::requestRead) != 0;
   const uint32_t value = read ? 0 : data;
   const uint32_t after = static_cast<uint32_t>(deviceCount_) - index_ - 1;
   start();
   // Select-DR-Scan, Capture-DR, Shift-DR
   putOnes(1, true);
   putOnes(2, false);
   // The devices before have a 1-bit BYPASS register.
   putOnes(index_, false);
   const uint32_t first = bits_;
   // RnW, A2, A3, then the data
   put((request >> 1) & 0x07u, 3, false);
   put(value, 31, false);
   // The last bit of the scan goes to Exit1-DR.
   put(value >> 31, 1, after == 0);
   if(after != 0) {
      putOnes(after - 1, false);
      putOnes(1, true);
   }
   // Update-DR, Run-Test/Idle, and the idle cycles
   putOnes(1, true);
   putOnes(1u + idleCycles_, false);
   if(!run()) {
      return dap::ackNone;
   }
   // ACK[2:0] of the JTAG-DP, first bit first: OK/FAULT is 0b010, WAIT is
   // 0b001.
   const uint32_t ack = tdoBits(first, 3);
   if(read) {
      data = tdoBits(first + 3, 32);
   }
   return static_cast<uint8_t>(((ack & 1u) << 1) | ((ack >> 1) & 1u) | (ack & 4u));
}

uint8_t JtagDp::scanWithRetries(uint8_t request, uint32_t &data) {
   uint16_t retries = waitRetries_;
   while(true) {
      const uint8_t ack = scan(request, data);
      if(ack != dap::ackWait || retries == 0) {
         return ack;
      }
      --retries;
   }
}

uint8_t JtagDp::transfer(uint8_t request, uint32_t &data) {
   if(!setIr((request & dap::requestApNotDp) != 0 ? jtagIr::apacc : jtagIr::dpacc)) {
      return dap::ackNone;
   }
   const uint8_t ack = scanWithRetries(request, data);
   const bool dpRead = (request & (dap::requestApNotDp | dap::requestRead)) == dap::requestRead;
   if(ack != dap::ackOk || !dpRead || (request & dap::requestAddressMask) == (dap::readRdbuff & dap::requestAddressMask)) {
      return ack;
   }
   // The data of the DP read comes with the next scan.
   return scanWithRetries(dap::readRdbuff, data);
}

void JtagDp::writeAbort(uint32_t data) {
   if(setIr(jtagIr::abort)) {
      // A write to address 0. The acknowledge has no meaning.
      static_cast<void>(scan(0, data));
   }
}

bool JtagDp::readIdcode(uint32_t &value) {
   if(!setIr(jtagIr::idcode)) {
      return false;
   }
   const uint32_t after = static_cast<uint32_t>(deviceCount_) - index_ - 1;
   start();
   // Select-DR-Scan, Capture-DR, Shift-DR
   putOnes(1, true);
   putOnes(2, false);
   putOnes(index_, false);
   const uint32_t first = bits_;
   putOnes(31, false);
   putOnes(1, after == 0);
   if(after != 0) {
      putOnes(after - 1, false);
      putOnes(1, true);
   }
   // Update-DR, Run-Test/Idle
   putOnes(1, true);
   putOnes(1, false);
   if(!run()) {
      return false;
   }
   value = tdoBits(first, 32);
   return true;
}

bool JtagDp::sequence(uint8_t bitCount, bool tms, art::span<const uint8_t> tdi, art::span<uint8_t> tdo) {
   invalidate();
   start();
   for(uint32_t done = 0; done < bitCount; done += 8) {
      put(tdi[done / 8], std::min<uint32_t>(8, bitCount - done), tms);
   }
   if(!run()) {
      return false;
   }
   const std::size_t bytes = protocol::jtagRaw::byteCount(bitCount);
   std::copy_n(tdo_.begin(), std::min(bytes, tdo.size()), tdo.begin());
   return true;
}

bool JtagDp::tmsSequence(uint16_t bitCount, art::span<const uint8_t> data) {
   invalidate();
   start();
   for(uint32_t bit = 0; bit < bitCount; ++bit) {
      putOnes(1, ((data[bit / 8] >> (bit % 8)) & 1u) != 0);
   }
   return run();
}

} // namespace cmsisdap
