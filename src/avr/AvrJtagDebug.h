// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AVR JTAG debug mode: attach, halt, run, step, status and reset on the
// OCD low level (avr/AvrJtagOcd.h), with the sequences of the reference
// debugger (docs/avr-jtag-ocd.md, "Debug operations"). Hardware-
// independent: the host unit tests run it on mock pins with the OCD model
// of tests/MockAvrJtag.h.
//
// - attach(): TAP reset, IDCODE, then reg D. When the OCD does not answer
//   (OCDEN not programmed), attach() still succeeds: Bloom
//   (manage_ocden_fuse_bit) writes the fuse only after ATTACH. When the
//   OCD answers, attach() halts the core.
// - Each OCD operation reads reg D first when the OCD did not answer
//   before. No answer: NoOcd, which the scope sends as FAILED 0x50
//   ("Device not under control - check OCDEN fuse bit" in Bloom).
// - The probe keeps a copy of the BCR value it wrote last. The sequences
//   compute the next BCR from it, as the reference debugger does.
// - A wait for a stop reads reg D until bit 2 is set, at most
//   jtagDebug::stopTimeoutMs at the TCK frequency of the TAP.
// - The CPU state (context write-back before run and step) is not part of
//   this class yet.

#include <cstdint>

#include "avr/AvrJtagOcd.h"
#include "avr/AvrJtagTap.h"

namespace avr {

enum class JtagDebugStatus : uint8_t {
   Ok,
   // No TAP, or the IDCODE is not an Atmel IDCODE
   NoTap,
   // The OCD does not answer (OCDEN, or a failed scan)
   NoOcd,
   // The core did not stop in the time limit
   Timeout,
};

namespace jtagDebug {

// Longer than the 70 ms of a FORCE_BREAK in the captures and than the
// start-up time of a reset (65 ms and more)
inline constexpr uint32_t stopTimeoutMs = 300;
// TCK cycles of one OCD register read: two scans of 3 + 21 + 2
inline constexpr uint32_t readScanCycles = 2 * (3 + jtagOcd::registerBits + 2);

// The BCR values of docs/avr-jtag-ocd.md, "Debug operations"
inline constexpr uint16_t keepMask = 0x9FFC;
inline constexpr uint16_t haltClear = 0x03E0;
inline constexpr uint16_t idle = jtagOcd::bcr::bit15 | jtagOcd::bcr::bit14;
// Reset: reg 3 with mode 3, masked by reg 2 = 0, matches all addresses.
inline constexpr uint16_t resetBreak =
idle | jtagOcd::bcr::maskMode | jtagOcd::bcr::enable3 | (jtagOcd::bcr::modeProgram << jtagOcd::bcr::mode3Shift);
static_assert(resetBreak == 0xC298);

} // namespace jtagDebug

class AvrJtagDebug {
   public:
   AvrJtagDebug(AvrJtagTap &tap, AvrJtagOcd &ocd)
      : tap_(tap)
      , ocd_(ocd) {
   }

   // Enters debug mode. Ok also without an OCD answer (see above).
   [[nodiscard]] JtagDebugStatus attach() {
      ocdAnswers_ = false;
      stopped_ = false;
      uint32_t idcode = 0;
      if(!tap_.reset() || !tap_.readIdcode(idcode) || !tap::isAtmelIdcode(idcode)) {
         return JtagDebugStatus::NoTap;
      }
      uint16_t control = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
         return JtagDebugStatus::Ok;
      }
      ocdAnswers_ = true;
      return halt();
   }

   // True when the OCD answered since attach()
   [[nodiscard]] bool ocdAnswers() const {
      return ocdAnswers_;
   }

   // True after a stop that the probe saw: halt(), step(), reset() or
   // status()
   [[nodiscard]] bool isStopped() const {
      return stopped_;
   }

   // Stops the core. A stopped core stays stopped.
   [[nodiscard]] JtagDebugStatus halt() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      uint16_t value = 0;
      if(!ocd_.readRegister(jtagOcd::reg::bcr, value) || !writeBcr(static_cast<uint16_t>(keep(value) | jtagOcd::bcr::bit14)) ||
         !ocd_.readRegister(jtagOcd::reg::control, value) || !ocd_.forceBreak()) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!writeBcr(static_cast<uint16_t>(bcr_ & ~jtagDebug::haltClear)) || !writeBcr(jtagDebug::idle)) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = true;
      return JtagDebugStatus::Ok;
   }

   // Starts the core.
   [[nodiscard]] JtagDebugStatus run() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = false;
      return JtagDebugStatus::Ok;
   }

   // Executes one instruction and waits for the stop.
   [[nodiscard]] JtagDebugStatus step() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!writeBcr(static_cast<uint16_t>(keep(bcr_) | jtagOcd::bcr::step)) || !ocd_.run()) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = false;
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      return afterStop();
   }

   // Reads reg D one time. stopped gets bit 2. A new stop (a breakpoint)
   // gets the "after each stop" sequence.
   [[nodiscard]] JtagDebugStatus status(bool &stopped) {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      uint16_t control = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
         return JtagDebugStatus::NoOcd;
      }
      stopped = (control & jtagOcd::control::stopped) != 0;
      if(stopped && !stopped_) {
         return afterStop();
      }
      return JtagDebugStatus::Ok;
   }

   // Resets the core. It stops at the first instruction (the reset vector).
   // The probe does not send the reg 9 write of the reference debugger
   // (docs/avr-jtag-ocd.md).
   [[nodiscard]] JtagDebugStatus reset() {
      if(JtagDebugStatus status = requireOcd(); status != JtagDebugStatus::Ok) {
         return status;
      }
      stopped_ = false;
      if(!writeBcr(jtagDebug::idle) || !ocd_.reset(true) || !writeBcr(jtagDebug::resetBreak) ||
         !ocd_.writeRegister(jtagOcd::reg::break2, 0) || !ocd_.writeRegister(jtagOcd::reg::break3, 0)) {
         return JtagDebugStatus::NoOcd;
      }
      // Reg 6 and 7: address bits 23..16 of reg 2 and reg 3, revision 4 only
      if(ocd_.revision() >= 4 &&
         (!ocd_.writeRegister(jtagOcd::reg::breakHigh2, 0) || !ocd_.writeRegister(jtagOcd::reg::breakHigh3, 0))) {
         return JtagDebugStatus::NoOcd;
      }
      if(!ocd_.reset(false)) {
         return JtagDebugStatus::NoOcd;
      }
      if(JtagDebugStatus status = waitStop(); status != JtagDebugStatus::Ok) {
         return status;
      }
      if(!writeBcr(jtagDebug::idle)) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = true;
      return JtagDebugStatus::Ok;
   }

   private:
   static constexpr uint16_t keep(uint16_t value) {
      return static_cast<uint16_t>(value & jtagDebug::keepMask);
   }

   // Reads reg D when the OCD did not answer yet: OCDEN can change after
   // attach().
   [[nodiscard]] JtagDebugStatus requireOcd() {
      uint16_t control = 0;
      if(!ocdAnswers_ && ocd_.readRegister(jtagOcd::reg::control, control)) {
         ocdAnswers_ = true;
      }
      return ocdAnswers_ ? JtagDebugStatus::Ok : JtagDebugStatus::NoOcd;
   }

   [[nodiscard]] bool writeBcr(uint16_t value) {
      if(!ocd_.writeRegister(jtagOcd::reg::bcr, value)) {
         return false;
      }
      bcr_ = value;
      return true;
   }

   [[nodiscard]] JtagDebugStatus waitStop() {
      const uint32_t polls = jtagDebug::stopTimeoutMs * tap_.kHz() / jtagDebug::readScanCycles + 1u;
      for(uint32_t i = 0; i < polls; ++i) {
         uint16_t control = 0;
         if(!ocd_.readRegister(jtagOcd::reg::control, control)) {
            return JtagDebugStatus::NoOcd;
         }
         if((control & jtagOcd::control::stopped) != 0) {
            return JtagDebugStatus::Ok;
         }
      }
      return JtagDebugStatus::Timeout;
   }

   // "After each stop": reg D, BCR = (BCR & 0x9FFC) | 0x4000, then BCR
   // again from the target with bit 2 clear.
   [[nodiscard]] JtagDebugStatus afterStop() {
      uint16_t value = 0;
      if(!ocd_.readRegister(jtagOcd::reg::control, value) || !writeBcr(static_cast<uint16_t>(keep(bcr_) | jtagOcd::bcr::bit14)) ||
         !ocd_.readRegister(jtagOcd::reg::bcr, value) || !writeBcr(static_cast<uint16_t>(value & ~(1u << 2)))) {
         return JtagDebugStatus::NoOcd;
      }
      stopped_ = true;
      return JtagDebugStatus::Ok;
   }

   AvrJtagTap &tap_;
   AvrJtagOcd &ocd_;
   uint16_t bcr_{ 0 };
   bool ocdAnswers_{ false };
   bool stopped_{ false };
};

} // namespace avr
