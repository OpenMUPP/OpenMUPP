// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Board definition types. Each board has one constexpr Definition in
// board/boards/<Board>.h; board/Board.h selects the one of the build.
//
// This header has no CMSIS dependency, so the host unit tests can check
// every board. board/BoardPins.h maps a Pin to the GPIO driver on target.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace board {

// MCU family of the dongle. The pin functions (ADC channels, timers, SPI
// pins of the JTAG fast path) and the reserved pins depend on it.
enum class Family : uint8_t {
   Stm32F1, // STM32F103 (RM0008)
   Stm32H5  // STM32H523/H533 (RM0481)
};

enum class Port : uint8_t { A, B, C, D, E, F, G, H };

// GPIO pin, e.g. Pin{ Port::B, 13 } for PB13. A structural type, so it can
// be a template argument (see board::FastPinOf).
struct Pin {
   Port port;
   uint8_t number;

   constexpr bool operator==(const Pin &) const = default;
};

// Level at which a signal is asserted (LED on, key down, USB connected, ...).
enum class Active : bool { Low = false, High = true };

struct Signal {
   Pin pin;
   Active active;

   // Pin level (true: high) for the asserted or the released signal.
   [[nodiscard]] constexpr bool levelFor(bool asserted) const {
      return (active == Active::High) == asserted;
   }

   constexpr bool operator==(const Signal &) const = default;
};

// JTAG and SWD pins. SWCLK is TCK and SWDIO is TMS.
// TMS/SWDIO has an output pin and an input pin. They can be the same pin.
struct DebugPins {
   Pin tck;
   Pin tmsOut;
   Pin tmsIn;
   Pin tdi;
   Pin tdo;
   std::optional<Pin> trst;
   Pin srst;
   std::optional<Pin> rtck;

   constexpr bool operator==(const DebugPins &) const = default;
};

// The JTAG fast path (SPI and DMA) needs TCK on SPI2 SCK (PB13), TDI on
// SPI2 MOSI (PB15), TDO on SPI2 MISO (PB14), and TMS on SPI1 MISO. SPI1 MISO
// is PB4 on the STM32F103 (SPI1 remap). On the STM32H5 it is PA6: PB4 can
// also be SPI1 MISO there, but the pin map of the H5 boards (#89) uses PB4
// for TRST. SPI1 is a slave and gets its clock from TCK through a board
// link to SPI1 SCK (PB3).
struct JtagFastPath {
   Pin tckLink;

   constexpr bool operator==(const JtagFastPath &) const = default;
};

struct Uart {
   uint8_t usart; // USARTn
   Pin tx;
   Pin rx;

   constexpr bool operator==(const Uart &) const = default;
};

// Target voltage (Vref) sense: Vref = ADC input voltage * divider.
// The pin must have an ADC channel (see adcChannelOf()).
struct VrefSense {
   Pin pin;
   uint8_t divider;

   constexpr bool operator==(const VrefSense &) const = default;
};

namespace detail {

// ADC1/ADC2 input channel of a pin on the STM32F103 (RM0008 11.3.1):
// PA0-PA7 are channels 0-7, PB0-PB1 are 8-9 and PC0-PC5 are 10-15.
constexpr std::optional<uint8_t> adcChannelOfF1(Pin pin) {
   switch(pin.port) {
   case Port::A:
      if(pin.number <= 7) {
         return pin.number;
      }
      break;
   case Port::B:
      if(pin.number <= 1) {
         return static_cast<uint8_t>(8 + pin.number);
      }
      break;
   case Port::C:
      if(pin.number <= 5) {
         return static_cast<uint8_t>(10 + pin.number);
      }
      break;
   default:
      break;
   }
   return std::nullopt;
}

// ADC1/ADC2 positive input (INPx) of a pin on the STM32H523/H533
// (DS14539 table 14, the pins of the 64-pin package).
constexpr std::optional<uint8_t> adcChannelOfH5(Pin pin) {
   constexpr std::array<uint8_t, 8> portA{ { 0, 1, 14, 15, 18, 19, 3, 7 } };
   constexpr std::array<uint8_t, 2> portB{ { 9, 5 } };
   constexpr std::array<uint8_t, 6> portC{ { 10, 11, 12, 13, 4, 8 } };
   switch(pin.port) {
   case Port::A:
      if(pin.number < portA.size()) {
         return portA[pin.number];
      }
      break;
   case Port::B:
      if(pin.number < portB.size()) {
         return portB[pin.number];
      }
      break;
   case Port::C:
      if(pin.number < portC.size()) {
         return portC[pin.number];
      }
      break;
   default:
      break;
   }
   return std::nullopt;
}

} // namespace detail

// ADC input channel of a pin. Empty when the pin has no ADC input.
constexpr std::optional<uint8_t> adcChannelOf(Family family, Pin pin) {
   return family == Family::Stm32F1 ? detail::adcChannelOfF1(pin) : detail::adcChannelOfH5(pin);
}

// One wire (SDI) of the WCH-Link personality: a timer makes the bits on
// the SWDIO output pin (board/BoardRvSwd.cpp, board/BoardRvSwdH5.cpp). The
// output channel is on the pin; the sample channel has no pin output. The
// DMA channels serve the update event and the sample compare of the timer:
// DMA1 channels on the STM32F103, GPDMA1 channels on the STM32H5 (the
// GPDMA request comes from the timer and the channel).
struct OneWireTimer {
   uint8_t timer;         // TIMn
   uint8_t channel;       // output channel, 1-4
   bool complementary;    // the pin is TIMn_CHxN
   bool partialRemap;     // AFIO TIM3_REMAP partial remap
   uint8_t sampleChannel; // 1-4
   uint8_t updateDma;     // DMA channel of TIMn_UP
   uint8_t sampleDma;     // DMA channel of the sample channel
};

// The timer of a SWDIO output pin: RM0008 9.3.7 and table 78 (STM32F103),
// DS14540 table 14 (STM32H5). Empty when the pin has no supported timer
// channel.
constexpr std::optional<OneWireTimer> oneWireTimerOf(Family family, Pin tmsOut) {
   if(family == Family::Stm32H5) {
      // TIM3_CH1 (AF2), TIM3_CH3 samples. GPDMA1 channels 3 and 4: the JTAG
      // fast path uses channels 0 to 2.
      if(tmsOut == Pin{ Port::A, 6 }) {
         return OneWireTimer{ 3, 1, false, false, 3, 3, 4 };
      }
      return std::nullopt;
   }
   // TIM3_CH1 (partial remap); TIM3_UP on DMA1 ch3, TIM3_CH3 on ch2
   if(tmsOut == Pin{ Port::B, 4 }) {
      return OneWireTimer{ 3, 1, false, true, 3, 3, 2 };
   }
   // TIM1_CH2N (no remap); TIM1_UP on DMA1 ch5, TIM1_CH1 on ch2
   if(tmsOut == Pin{ Port::B, 14 }) {
      return OneWireTimer{ 1, 2, true, false, 1, 5, 2 };
   }
   return std::nullopt;
}

// Target power switch. Drive the pin to the active level to switch on.
// To switch off, the pin floats (input).
struct PowerSwitch {
   Pin pin;
   Active on;

   constexpr bool operator==(const PowerSwitch &) const = default;
};

struct Leds {
   std::optional<Signal> power;
   std::optional<Signal> state;
   std::optional<Signal> usb;
   // Two LEDs on the state pin, one to each supply: the asserted level
   // lights one colour, the other level the other colour (stateDriveOf()).
   bool stateTwoColour = false;

   constexpr bool operator==(const Leds &) const = default;
};

// What the state LED shows (docs/boards.md, "Start-up and LEDs").
enum class StateShow : uint8_t { Idle, Activity, Fault };

struct StateDrive {
   bool off;   // analog mode: no LED is on
   bool level; // output level when not off
   constexpr bool operator==(const StateDrive &) const = default;
};

// The pin drive of the state LED. One LED: off when idle, on for activity
// and a fault. Two colours: idle lights the asserted colour, a fault the
// other colour, and activity sets the pin to analog mode. A floating
// digital input lets one LED glow (measured on the mini clone); analog
// mode switches both off.
constexpr StateDrive stateDriveOf(Signal state, bool twoColour, StateShow show) {
   if(!twoColour) {
      return { false, state.levelFor(show != StateShow::Idle) };
   }
   if(show == StateShow::Activity) {
      return { true, false };
   }
   return { false, state.levelFor(show == StateShow::Idle) };
}

struct Definition {
   std::string_view name;
   // Crystal, Hz. 0: the board has no crystal (STM32H5 only, USB then runs
   // from the HSI48 with the CRS).
   uint32_t hseFrequency : 28;
   // In the word of the crystal, so the family adds no byte. The firmware
   // keeps board::current in flash (BoardGpio reads the debug pins).
   Family family : 4;
   uint8_t hardwareId; // reply of VERSALOON_GET_HARDWARE
   DebugPins debug;
   std::optional<JtagFastPath> jtagFastPath;
   std::optional<Uart> uart;
   std::optional<VrefSense> vref;
   std::optional<PowerSwitch> powerSwitch;
   Leds leds;
   std::optional<Signal> key;
   // Software-controlled USB D+ pull-up. Empty: the pull-up is fixed.
   std::optional<Signal> usbPullUp;
   // The pull-up pin is an open-drain output: the board sets the high level.
   bool usbPullUpOpenDrain = false;
   // Add each new pin field to detail::allPins() too, else isValid() and
   // swjConfig() do not see it.

   [[nodiscard]] constexpr bool hasJtagFastPath() const {
      return jtagFastPath.has_value();
   }

   constexpr bool operator==(const Definition &) const = default;
};

// Pins that no board signal can use: USB D- and D+ (PA11, PA12) and the
// crystal (PD0 and PD1 on the STM32F103, PH0 and PH1 on the STM32H5). The
// crystal pins stay reserved on a board without a crystal. The SWD pins of
// the dongle MCU (PA13, PA14) are not in this list, because a release
// build of the Mini uses PA14 for an LED.
constexpr std::array<Pin, 4> reservedPins(Family family) {
   const Port crystal = family == Family::Stm32F1 ? Port::D : Port::H;
   return { { { Port::A, 11 }, { Port::A, 12 }, { crystal, 0 }, { crystal, 1 } } };
}

namespace detail {

constexpr bool isValidPin(const Pin &pin) {
   return pin.number < 16;
}

// All pins of a definition. Unused optional pins are empty.
constexpr std::array<std::optional<Pin>, 18> allPins(const Definition &def) {
   auto pinOf = [](const auto &opt) -> std::optional<Pin> { return opt ? std::optional<Pin>(opt->pin) : std::nullopt; };
   return { {
   def.debug.tck,
   def.debug.tmsOut,
   // tmsIn can be the same pin as tmsOut (one bidirectional SWDIO pin).
   def.debug.tmsIn != def.debug.tmsOut ? std::optional<Pin>(def.debug.tmsIn) : std::nullopt,
   def.debug.tdi,
   def.debug.tdo,
   def.debug.trst,
   def.debug.srst,
   def.debug.rtck,
   def.jtagFastPath ? std::optional<Pin>(def.jtagFastPath->tckLink) : std::nullopt,
   def.uart ? std::optional<Pin>(def.uart->tx) : std::nullopt,
   def.uart ? std::optional<Pin>(def.uart->rx) : std::nullopt,
   pinOf(def.vref),
   pinOf(def.powerSwitch),
   pinOf(def.leds.power),
   pinOf(def.leds.state),
   pinOf(def.leds.usb),
   pinOf(def.key),
   pinOf(def.usbPullUp),
   } };
}

} // namespace detail

// True when the pins of the definition are valid: pin numbers 0-15, no pin
// used twice (TMS out and in can be one pin), no reserved pin, a Vref pin
// with an ADC channel, and the fixed fast path pins when the board has the
// JTAG fast path. The crystal must be in the range of the family, and an
// STM32F103 board must have one. Each board header checks its definition
// with a static_assert.
constexpr bool isValid(const Definition &def) {
   const auto pins = detail::allPins(def);
   for(std::size_t i = 0; i < pins.size(); ++i) {
      if(!pins[i]) {
         continue;
      }
      if(!detail::isValidPin(*pins[i])) {
         return false;
      }
      for(const auto &reserved : reservedPins(def.family)) {
         if(*pins[i] == reserved) {
            return false;
         }
      }
      for(std::size_t j = i + 1; j < pins.size(); ++j) {
         if(pins[j] && *pins[i] == *pins[j]) {
            return false;
         }
      }
   }
   if(def.jtagFastPath) {
      const auto &d = def.debug;
      const Pin spi1Miso = def.family == Family::Stm32F1 ? Pin{ Port::B, 4 } : Pin{ Port::A, 6 };
      if(d.tck != Pin{ Port::B, 13 } || d.tdi != Pin{ Port::B, 15 } || d.tdo != Pin{ Port::B, 14 } ||
         d.tmsOut != spi1Miso || def.jtagFastPath->tckLink != Pin{ Port::B, 3 }) {
         return false;
      }
   }
   if(def.vref && (!adcChannelOf(def.family, def.vref->pin) || def.vref->divider == 0)) {
      return false;
   }
   if(def.hseFrequency == 0) {
      return def.family != Family::Stm32F1;
   }
   // HSE crystal range: RM0008 7.2.1 (STM32F103), DS14539 (STM32H5)
   const uint32_t maxHse = def.family == Family::Stm32F1 ? 16'000'000 : 50'000'000;
   return def.hseFrequency >= 4'000'000 && def.hseFrequency <= maxHse;
}

// Serial wire JTAG (SWJ) debug port of the dongle MCU. On both families the
// JTAG-DP uses PA15 (JTDI), PB3 (JTDO) and PB4 (NJTRST), and the SW-DP uses
// PA13 (SWDIO) and PA14 (SWCLK). The STM32F103 frees the pins with
// AFIO_MAPR.SWJ_CFG (RM0008 9.3.5). The STM32H5 has no AFIO: the pins are
// AF0 after reset, and a GPIO mode frees each pin.
enum class SwjConfig : uint8_t {
   Full,    // JTAG-DP and SW-DP on
   NoJtag,  // SW-DP only: frees PA15, PB3 and PB4
   Disabled // no debug port: frees PA13 and PA14 too
};

// SW-DP pins of the dongle MCU: SWDIO (PA13) and SWCLK (PA14).
inline constexpr std::array<Pin, 2> swdPins{ { { Port::A, 13 }, { Port::A, 14 } } };

// The SWJ configuration that frees all pins of the definition and keeps as
// much of the debug port as possible.
constexpr SwjConfig swjConfig(const Definition &def) {
   constexpr std::array<Pin, 3> jtagOnlyPins{ { { Port::A, 15 }, { Port::B, 3 }, { Port::B, 4 } } };
   bool usesJtagOnlyPin = false;
   for(const auto &pin : detail::allPins(def)) {
      if(!pin) {
         continue;
      }
      for(const auto &swd : swdPins) {
         if(*pin == swd) {
            return SwjConfig::Disabled;
         }
      }
      for(const auto &jtag : jtagOnlyPins) {
         usesJtagOnlyPin = usesJtagOnlyPin || *pin == jtag;
      }
   }
   return usesJtagOnlyPin ? SwjConfig::NoJtag : SwjConfig::Full;
}

// The definition of a debug build (MUPP_DEBUG_SWD): the same board without
// the LEDs on the SW-DP pins, so swjConfig() keeps SWD on and a debug probe
// can run and step the firmware. The firmware works without these LEDs.
// Other signals on the SW-DP pins stay: board/Board.h checks that the
// result keeps SWD.
constexpr Definition withSwd(Definition def) {
   for(auto *led : { &def.leds.power, &def.leds.state, &def.leds.usb }) {
      for(const auto &swd : swdPins) {
         if(*led && (*led)->pin == swd) {
            led->reset();
         }
      }
   }
   return def;
}

} // namespace board
