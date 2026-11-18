// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The AVR JTAG target of the AVR8 scope (protocol/avr/Avr8Scope.h): the TAP
// layer (avr/AvrJtagTap.h) and the JTAG programming (avr/AvrJtagProg.h) on
// one JTAG port. Hardware-independent: the host unit tests run it on the
// JTAG bit-bang engine with mock pins.
//
// The scope uses the programming only: the debug mode (debug()) has no
// scope commands yet, so the target has no hardware breakpoints for the
// host.
//
// The caller owns the target pins from open() to close(): the EDBG
// transport takes them at ACTIVATE_PHYSICAL.

#include <cstddef>
#include <cstdint>

#include "avr/AvrJtagDebug.h"
#include "avr/AvrJtagOcd.h"
#include "avr/AvrJtagProg.h"
#include "avr/AvrJtagTap.h"
#include "protocol/JtagPort.h"

namespace avr {

// The device context of the host for a JTAG session
struct AvrJtagDevice {
   JtagProgDevice prog{};
   uint16_t eepromBytes{ 1024 };
   // 2 to 4 for the parts with a JTAG OCD (docs/avr-jtag-ocd.md)
   uint8_t ocdRevision{ 3 };
};

class AvrJtagTarget {
   public:
   // The traits for avr::Avr8Scope: the EDBG variant (2, mega) and physical
   // interface (4, JTAG), and a programming mode.
   static constexpr uint8_t avr8Variant = 0x02;
   static constexpr uint8_t avr8Physical = 0x04;
   static constexpr std::size_t hardwareBreaks = 0;
   static constexpr bool hasProgmode = true;

   explicit AvrJtagTarget(protocol::JtagPort &port)
      : tap_(port) {
   }

   void setDevice(const AvrJtagDevice &device) {
      device_ = device;
      ocd_.setRevision(device.ocdRevision);
   }
   [[nodiscard]] const AvrJtagDevice &device() const {
      return device_;
   }

   // The OCD stops the timers at a break. Not used in programming.
   void setRunTimers(bool) {
   }

   // Drives the pins at kHz and resets the TAP. False: the pins are
   // released.
   [[nodiscard]] bool open(uint32_t kHz) {
      return tap_.open(kHz);
   }

   // Leaves the programming mode and releases the pins. False when the
   // programming mode did not end cleanly (the pins are released also
   // then).
   bool close() {
      const bool left = !prog_.programming() || prog_.leave();
      tap_.close();
      return left;
   }

   [[nodiscard]] bool setSpeed(uint32_t kHz) {
      return tap_.setSpeed(kHz);
   }

   [[nodiscard]] bool readIdcode(uint32_t &idcode) {
      return tap_.readIdcode(idcode);
   }

   [[nodiscard]] AvrJtagProg &prog() {
      return prog_;
   }

   [[nodiscard]] AvrJtagDebug &debug() {
      return debug_;
   }

   private:
   AvrJtagTap tap_;
   AvrJtagProg prog_{ tap_ };
   AvrJtagOcd ocd_{ tap_ };
   AvrJtagDebug debug_{ tap_, ocd_ };
   AvrJtagDevice device_{};
};

} // namespace avr
