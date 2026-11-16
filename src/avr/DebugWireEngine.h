// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// debugWIRE (dW) physical layer: the one-wire serial line on the RESET pin
// of the AVR. The line is open-drain with the pull-up of the target (10 kOhm
// or more). Idle is high. A frame is UART 8N1, LSB first, at the target
// clock / 128 after a reset of the target. The probe and the target send on
// the same line, so the probe receives each byte that it sends (the echo).
//
// - TX: software on the RESET drive pin, with the core cycle counter. The
//   interrupts are off for one byte (10 bit times, 64 us at 156 kbaud).
// - RX: a UART receiver on the read-back pin of the line. It also receives
//   the echo, and send() compares it with the sent byte (collision check).
// - Break: the probe holds the line low. The target stops and sends the
//   sync byte 0x55. A target that stops (breakpoint, step, reset) sends a
//   break and 0x55: the receiver sees 0x00, usually with a framing error.
// - Baud rate: sync() measures the 0x55 on the line with the cycle counter.
//   0x55 with the start and the stop bit gives 10 bits that change at each
//   bit. The time from the first rising edge to the last rising edge is 8
//   bit times.
//
// Hardware-independent: a Pins type gives the access, so the host unit
// tests run the engine on a mock dW target. On target, board/BoardDebugWire.h
// gives it. The Pins type has these members:
//
//   void init();               the line released, the read-back pin an
//                              input, the receiver off
//   void fini();               the line released, the pins inputs, the
//                              receiver off
//   void begin();              before each operation: make now() count
//   void drive(bool low);      true pulls the line low. The board sets the
//                              pin level (polarity).
//   bool sense();              the level of the line now (true: high)
//   uint32_t now();            free-running core cycle counter
//   uint32_t coreHz();         core clock frequency
//   uint32_t lockIrq();        interrupts off, returns the old state
//   void unlockIrq(uint32_t);  restores the state of lockIrq()
//   void rxEnable(uint32_t baud);
//                              the receiver on at baud, with an empty buffer
//   void rxDisable();
//   int32_t rxRead();          one received byte, or -1 for none. Bit 8
//                              (dwFramingError) is set when the stop bit
//                              was low.
//
// With a protocol::Background, the engine calls Background::service() in
// its waits and before each sent byte, at most once per
// backgroundIntervalMs (protocol/Background.h). The line is then idle or
// held low for a break. The receive functions do not call it: the receiver
// holds only one byte, and a long service() would lose the next one.

#include <array>
#include <cstdint>

#include "protocol/Background.h"

namespace avr {

enum class DwStatus : uint8_t {
   Ok,
   // No valid 0x55 after the break, or no break and 0x55 from the target
   NoSync,
   // send() or receive() before a sync()
   NotSynced,
   // The echo of a sent byte is wrong or missing: the target sent at the
   // same time, or the line does not follow the drive pin.
   Collision,
   // No byte in the time limit
   Timeout,
   // receive(): the target sent a break (0x00 with a framing error).
   Break,
   // receive(): a byte other than 0x00 with a framing error
   FramingError,
};

// Bit 8 of Pins::rxRead(): the stop bit was low.
inline constexpr int32_t dwFramingError = 0x100;

// The baud rate range of sync(). 500 baud is a 64 kHz target clock, 1 Mbaud
// is above the 156 kbaud of a 20 MHz target.
inline constexpr uint32_t dwMinBaud = 500;
inline constexpr uint32_t dwMaxBaud = 1'000'000;

// The break before the first sync: longer than one frame at dwMinBaud
// (20 ms). After a sync, the break is dwBreakBits bit times.
inline constexpr uint32_t dwFirstBreakUs = 30'000;
inline constexpr uint32_t dwBreakBits = 40;

// The wait for the start bit of the 0x55 after a break
inline constexpr uint32_t dwSyncTimeoutUs = 50'000;

// The default time limit of receive() for each byte, and the time limit of
// the 0x55 after a target break (bit times)
inline constexpr uint32_t dwByteTimeoutBits = 40;

template <typename Pins>
class DebugWireEngine {
   public:
   // background: serviced in the waits and between sent bytes, nullptr for
   // none.
   explicit DebugWireEngine(Pins &pins, protocol::Background *background = nullptr)
      : pins_(pins)
      , background_(background) {
   }

   // The line released, the receiver off. Call sync() next.
   void connect() {
      bitCycles_ = 0;
      pins_.init();
   }

   void disconnect() {
      bitCycles_ = 0;
      pins_.fini();
   }

   // Sends a break, measures the 0x55 of the target and switches the
   // receiver on at its baud rate. NoSync: no target, or a wrong pattern.
   // Then the engine is not synced.
   [[nodiscard]] DwStatus sync() {
      begin();
      const uint32_t breakCycles = synced() ? dwBreakBits * bitCycles_ : usCycles(dwFirstBreakUs);
      bitCycles_ = 0;
      pins_.rxDisable();
      pins_.drive(true);
      waitCycles(breakCycles);
      pins_.drive(false);
      const uint32_t total = measureSync();
      if(total == 0) {
         return DwStatus::NoSync;
      }
      bitCycles_ = (total + 4u) / 8u;
      baud_ = static_cast<uint32_t>((uint64_t{ pins_.coreHz() } * 8u + total / 2u) / total);
      // The end of the stop bit
      waitCycles(2u * bitCycles_);
      pins_.rxEnable(baud_);
      return DwStatus::Ok;
   }

   // Sends bytes with the collision check. Collision: the echo of a byte
   // is wrong or missing. The bytes after it are not sent.
   [[nodiscard]] DwStatus send(const uint8_t *data, uint32_t bytes) {
      if(!synced()) {
         return DwStatus::NotSynced;
      }
      begin();
      for(uint32_t i = 0; i < bytes; ++i) {
         static_cast<void>(background_.poll(pins_));
         const uint32_t irq = pins_.lockIrq();
         sendByte(data[i]);
         pins_.unlockIrq(irq);
         // The receiver has the echo at the middle of the stop bit. The
         // margin is for the rounding of its baud rate.
         if(readByte(4u * bitCycles_) != data[i]) {
            return DwStatus::Collision;
         }
      }
      return DwStatus::Ok;
   }

   // Receives bytes. timeoutUs is the limit for each byte, 0 for
   // dwByteTimeoutBits bit times. Break: the target sent a break; the 0x55
   // after it is still in the receiver (waitBreak() reads it).
   [[nodiscard]] DwStatus receive(uint8_t *data, uint32_t bytes, uint32_t timeoutUs = 0) {
      if(!synced()) {
         return DwStatus::NotSynced;
      }
      begin();
      const uint32_t timeout = timeoutUs == 0 ? dwByteTimeoutBits * bitCycles_ : usCycles(timeoutUs);
      for(uint32_t i = 0; i < bytes; ++i) {
         const int32_t value = readByte(timeout);
         if(value < 0) {
            return DwStatus::Timeout;
         }
         if((value & dwFramingError) != 0) {
            return (value & 0xFF) == 0 ? DwStatus::Break : DwStatus::FramingError;
         }
         data[i] = static_cast<uint8_t>(value);
      }
      return DwStatus::Ok;
   }

   // Waits up to timeoutUs for the break and the 0x55 of a target that
   // stops. 0 checks one time: for a poll while the target runs. Timeout:
   // no byte. NoSync: another byte than the break, or no 0x55 after it.
   [[nodiscard]] DwStatus waitBreak(uint32_t timeoutUs) {
      if(!synced()) {
         return DwStatus::NotSynced;
      }
      begin();
      const uint32_t timeout = usCycles(timeoutUs);
      const uint32_t start = pins_.now();
      int32_t value = pins_.rxRead();
      while(value < 0 && pins_.now() - start < timeout) {
         static_cast<void>(background_.poll(pins_));
         value = pins_.rxRead();
      }
      if(value < 0) {
         return DwStatus::Timeout;
      }
      if((value & 0xFF) != 0) {
         return DwStatus::NoSync;
      }
      return readByte(dwByteTimeoutBits * bitCycles_) == 0x55 ? DwStatus::Ok : DwStatus::NoSync;
   }

   // Drops the received bytes.
   void flush() {
      while(pins_.rxRead() >= 0) {
      }
   }

   // us up to about 29 s at 144 MHz (the 32-bit counter)
   void waitUs(uint32_t us) {
      begin();
      waitCycles(usCycles(us));
   }

   [[nodiscard]] bool synced() const {
      return bitCycles_ != 0;
   }

   // Core cycles of one bit, 0 before sync()
   [[nodiscard]] uint32_t bitCycles() const {
      return bitCycles_;
   }

   // The measured baud rate of the target, valid after sync()
   [[nodiscard]] uint32_t baud() const {
      return baud_;
   }

   private:
   // The core clock can change after the constructor (clock setup).
   void begin() {
      pins_.begin();
      background_.begin(pins_);
   }

   [[nodiscard]] uint32_t usCycles(uint32_t us) {
      return pins_.coreHz() / 1'000'000u * us;
   }

   void waitCycles(uint32_t cycles) {
      const uint32_t start = pins_.now();
      while(pins_.now() - start < cycles) {
         static_cast<void>(background_.poll(pins_));
      }
   }

   // Waits for the line level, up to cycles. False on the time limit.
   bool waitLevel(bool high, uint32_t cycles) {
      const uint32_t start = pins_.now();
      while(pins_.sense() != high) {
         if(pins_.now() - start >= cycles) {
            return false;
         }
      }
      return true;
   }

   // The 0x55 after the release of a break: the cycles of 8 bits from the
   // first to the last rising edge, 0 for no valid pattern. The wait for
   // the start bit lets interrupts run between two reads of the line. An
   // interrupt can thus make the start bit late, but the measurement starts
   // only at the next edge, with the interrupts off.
   uint32_t measureSync() {
      const uint32_t maxBit = 2u * (pins_.coreHz() / dwMinBaud);
      const uint32_t minBit = pins_.coreHz() / dwMaxBaud;
      // The line must rise first: the pull-up needs some time.
      if(!waitLevel(true, maxBit)) {
         return 0;
      }
      const uint32_t timeout = usCycles(dwSyncTimeoutUs);
      const uint32_t start = pins_.now();
      uint32_t irq = 0;
      for(;;) {
         irq = pins_.lockIrq();
         if(!pins_.sense()) {
            break;
         }
         pins_.unlockIrq(irq);
         if(pins_.now() - start >= timeout) {
            return 0;
         }
      }
      // Rising edges at bits 1, 3, 5, 7 and 9, falling edges at 2, 4, 6
      // and 8
      std::array<uint32_t, 9> edges{};
      bool ok = true;
      for(uint32_t i = 0; i < 9 && ok; ++i) {
         ok = waitLevel(i % 2u == 0, maxBit);
         edges[i] = pins_.now();
      }
      pins_.unlockIrq(irq);
      if(!ok) {
         return 0;
      }
      const uint32_t total = edges[8] - edges[0];
      if(total < 8u * minBit || total > 8u * (maxBit / 2u)) {
         return 0;
      }
      // Each bit within half a bit of the mean: 0x55, not noise or a
      // different byte
      for(uint32_t i = 0; i < 8; ++i) {
         const uint32_t bit = (edges[i + 1] - edges[i]) * 16u;
         if(bit < total || bit > 3u * total) {
            return 0;
         }
      }
      return total;
   }

   // One frame, with the interrupts off. Each bit edge comes at its
   // deadline from the start, so the bit times do not add up errors.
   void sendByte(uint8_t value) {
      // Start bit 0, the data LSB first, stop bit 1
      uint32_t frame = (uint32_t{ value } << 1) | 0x200u;
      const uint32_t start = pins_.now();
      uint32_t deadline = 0;
      for(uint32_t bit = 0; bit < 10; ++bit) {
         pins_.drive((frame & 1u) == 0);
         frame >>= 1;
         deadline += bitCycles_;
         while(pins_.now() - start < deadline) {
         }
      }
   }

   int32_t readByte(uint32_t timeout) {
      const uint32_t start = pins_.now();
      int32_t value = pins_.rxRead();
      while(value < 0 && pins_.now() - start < timeout) {
         value = pins_.rxRead();
      }
      return value;
   }

   Pins &pins_;
   protocol::BackgroundTimer background_;
   uint32_t bitCycles_{ 0 };
   uint32_t baud_{ 0 };
};

} // namespace avr
