// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The drive of the target pins: the order of the level shifter direction
// pins (board::Shifters) against the MCU pins of an engine and of the gpio
// requests, and the SRST pin
// with the polarity of DebugPins::srstActive. Hardware-independent: the
// host unit tests check it. board/BoardTargetDrive.h applies it to
// board::current on target.

#include <array>
#include <optional>

#include "board/BoardDefinition.h"
#include "board/DebugGpio.h"
#include "protocol/GpioPins.h"

namespace board {

// The shifter groups that an engine drives. Out: TCK, TDI, TRST, AUX1 and
// AUX2 (DIR_OUT). OutAndSwdio: also TMS / SWDIO (DIR_SWDIO), for JTAG, where
// TMS is always an output.
enum class ShifterGroups : uint8_t { Out, OutAndSwdio };

// The gpio bits (protocol/GpioPins.h) of the DIR_OUT group
inline constexpr uint16_t dirOutGpioBits = protocol::gpio::tck | protocol::gpio::tdi | protocol::gpio::trst;

// The users that keep DIR_OUT high. DIR_OUT is high while one of them uses
// it. Thus all MCU pins of the DIR_OUT group are outputs while it is high.
struct DirOutUsers {
   // An engine: driveTargetOutputs() on, until driveTargetOutputs() off
   bool engine = false;
   // The gpio bits of the DIR_OUT group that driveGpioPin() made outputs
   uint16_t gpio = 0;

   [[nodiscard]] constexpr bool any() const {
      return engine || gpio != 0;
   }
};

namespace detail {

struct IdlePin {
   Pin pin;
   bool level;
};

// The pins of the DIR_OUT group that get a level before DIR_OUT goes high:
// TRST high (released), TDI high, AUX1 and AUX2 low. With tck, also TCK
// low: an engine sets TCK itself, the gpio does not.
constexpr std::array<std::optional<IdlePin>, 5> dirOutIdlePins(const DebugPins &pins, const Shifters &shifters, bool tck) {
   return { {
   tck ? std::optional<IdlePin>(IdlePin{ pins.tck, false }) : std::nullopt,
   pins.trst ? std::optional<IdlePin>(IdlePin{ *pins.trst, true }) : std::nullopt,
   IdlePin{ pins.tdi, true },
   IdlePin{ shifters.aux1, false },
   IdlePin{ shifters.aux2, false },
   } };
}

} // namespace detail

// The connect self-test: DIR_OUT is high, and TCK and TDI are MCU outputs.
// Each of the two pins goes to the inverse of its level and back. After
// each step both read-back inputs (connector side) must show the levels of
// TCK and TDI. A mismatch shows a short, or a target that drives against
// the probe. The test gives one pulse on TCK and on TDI. At that time JTAG
// holds TMS high (the pulse moves the TAP to Test-Logic-Reset), SWD and
// RVSWD have SWDIO released (no start condition), and ISP holds RESET.
template <class Io>
bool connectSelfTest(const DebugPins &pins, const Shifters &shifters, Io &io) {
   struct Line {
      Pin pin;
      Pin readBack;
      bool level;
   };
   std::array<Line, 2> lines{ {
   { pins.tck, shifters.tckReadBack, io.read(pins.tck) },
   { pins.tdi, shifters.tdiReadBack, io.read(pins.tdi) },
   } };
   bool ok = true;
   for(Line &line : lines) {
      for(int step = 0; step < 2; ++step) {
         line.level = !line.level;
         io.write(line.pin, line.level);
         io.settle();
         for(const Line &check : lines) {
            ok = ok && io.read(check.readBack) == check.level;
         }
      }
   }
   return ok;
}

// Drives the target outputs of an engine (on) or releases them (off).
// Without shifters, only setMcuPins() runs. With shifters:
// - on: first the pins of the DIR_OUT group get a defined output level,
//   because DIR_OUT high also drives the pins that the engine does not use:
//   TRST high (released), TDI high, AUX1 and AUX2 low. Then setMcuPins()
//   sets the pins of the engine, and then the direction pins of groups go
//   high. When DIR_OUT is high already (users), the group pins are outputs
//   and keep their levels: a TRST that DAP_SWJ_Pins asserted stays
//   asserted.
// - off: both direction pins go low first (low is the safe state of each),
//   then setMcuPins() sets the pins of the engine to inputs, and then the
//   pins of the first step become inputs again. Also the gpio outputs of
//   the group are inputs then.
// Thus a shifter does not drive the target from a floating MCU pin, and an
// MCU output does not drive against a shifter. Io has direction(pin, level)
// (write a direction pin), output(pin, level), input(pin), and for the
// self-test write(pin, level), read(pin) and settle().
// When on sets DIR_OUT high, the self-test of the read-back pins runs
// (connectSelfTest()). The result is false when it fails, else true.
template <class Io, class SetMcuPins>
bool driveTargetOutputs(const DebugPins &pins, const std::optional<Shifters> &shifters, DirOutUsers &users, ShifterGroups groups, bool on, Io &io, SetMcuPins &&setMcuPins) {
   if(!shifters) {
      setMcuPins();
      return true;
   }
   const auto idle = detail::dirOutIdlePins(pins, *shifters, false);
   if(on) {
      const bool high = users.any();
      if(!high) {
         for(const auto &pin : idle) {
            if(pin) {
               io.output(pin->pin, pin->level);
            }
         }
      }
      setMcuPins();
      users.engine = true;
      if(!high) {
         io.direction(shifters->dirOut, true);
      }
      if(groups == ShifterGroups::OutAndSwdio) {
         io.direction(shifters->dirSwdio, true);
      }
      return high || connectSelfTest(pins, *shifters, io);
   }
   io.direction(shifters->dirOut, false);
   io.direction(shifters->dirSwdio, false);
   setMcuPins();
   const bool gpioTck = (users.gpio & protocol::gpio::tck) != 0;
   for(const auto &pin : detail::dirOutIdlePins(pins, *shifters, gpioTck)) {
      if(pin) {
         io.input(pin->pin);
      }
   }
   users = {};
   return true;
}

// The pin mode of a gpio request. Behind a shifter an output is push-pull:
// an open-drain high would leave the shifter input floating.
constexpr protocol::PinMode gpioPinMode(const std::optional<Shifters> &shifters, uint16_t bit, protocol::PinMode mode) {
   const bool shifted = (bit & (dirOutGpioBits | protocol::gpio::tms)) != 0;
   return shifters && shifted && mode == protocol::PinMode::OpenDrain ? protocol::PinMode::PushPull : mode;
}

// A gpio request on one pin (DAP_SWJ_Pins, the release at DAP_Disconnect,
// the Versaloon GPIO block). setPin() sets the MCU pin as requested. Without
// shifters, or for a pin without shifter direction (SRST, TDO, RTCK), only
// setPin() runs. With shifters:
// - TMS: an output gets DIR_SWDIO high after setPin(), an input gets it low
//   before setPin().
// - An output in the DIR_OUT group: when DIR_OUT is low, the other pins of
//   the group get the idle levels of driveTargetOutputs() and TCK low, then
//   setPin() runs and DIR_OUT goes high.
// - An input in the DIR_OUT group while DIR_OUT is high: when no other user
//   keeps DIR_OUT high, DIR_OUT goes low, the other pins of the group become
//   inputs and setPin() runs. Else the pin cannot be an input: it gets its
//   released level as an output (TCK low, TDI and TRST high).
template <class Io, class SetPin>
void driveGpioPin(const DebugPins &pins, const std::optional<Shifters> &shifters, DirOutUsers &users, uint16_t bit, bool output, Io &io, SetPin &&setPin) {
   if(shifters && bit == protocol::gpio::tms) {
      if(!output) {
         io.direction(shifters->dirSwdio, false);
      }
      setPin();
      if(output) {
         io.direction(shifters->dirSwdio, true);
      }
      return;
   }
   if(!shifters || (bit & dirOutGpioBits) == 0) {
      setPin();
      return;
   }
   const auto ownPin = gpioOutputPin(pins, bit);
   if(!ownPin) {
      return;
   }
   const Pin own = *ownPin;
   const auto others = detail::dirOutIdlePins(pins, *shifters, true);
   const bool high = users.any();
   if(output) {
      if(!high) {
         for(const auto &pin : others) {
            if(pin && pin->pin != own) {
               io.output(pin->pin, pin->level);
            }
         }
      }
      setPin();
      users.gpio = static_cast<uint16_t>(users.gpio | bit);
      if(!high) {
         io.direction(shifters->dirOut, true);
      }
      return;
   }
   users.gpio = static_cast<uint16_t>(users.gpio & ~bit);
   if(!high) {
      setPin();
      return;
   }
   if(users.any()) {
      io.output(own, bit != protocol::gpio::tck);
      return;
   }
   io.direction(shifters->dirOut, false);
   for(const auto &pin : others) {
      if(pin && pin->pin != own) {
         io.input(pin->pin);
      }
   }
   setPin();
}

// The setting of the SRST pin.
struct SrstPinSetting {
   protocol::PinMode mode;
   bool level;

   constexpr bool operator==(const SrstPinSetting &) const = default;
};

// The SRST pin setting for a request on the SRST gpio bit. The CMSIS-DAP
// processor and the Versaloon GPIO block see SRST as nRESET: open-drain and
// active low, a low level asserts the reset, an input releases it. With
// Active::Low, the pin is nRESET and the request stays. With Active::High,
// the pin drives a transistor that pulls nRESET low: the pin is a push-pull
// output with the inverse level, and a released line (an input request) is
// a low output, not an input with pull-up.
constexpr SrstPinSetting srstPinSetting(Active active, protocol::PinMode mode, bool level) {
   if(active == Active::Low) {
      return { mode, level };
   }
   const bool output = mode == protocol::PinMode::PushPull || mode == protocol::PinMode::OpenDrain;
   return { protocol::PinMode::PushPull, output && !level };
}

// The pin that reads nRESET. Active::Low: the SRST pin is nRESET. On the
// YD board the read-back input needs a wire that only debugWIRE uses, so
// it does not read SRST. Active::High: the read-back input when the board
// has it, else the SRST pin.
constexpr Pin srstReadPin(const DebugPins &pins) {
   return pins.srstActive == Active::High ? pins.srstSense.value_or(pins.srst) : pins.srst;
}

// The nRESET level from the level of srstReadPin(). Without a read-back
// input, the SRST pin of an Active::High board has the inverse level of
// nRESET.
constexpr bool srstLineLevel(const DebugPins &pins, bool pinLevel) {
   return srstReadPin(pins) == pins.srst && pins.srstActive == Active::High ? !pinLevel : pinLevel;
}

} // namespace board
