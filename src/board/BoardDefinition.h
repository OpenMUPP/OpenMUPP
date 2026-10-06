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

enum class Port : uint8_t { A, B, C, D };

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
};

// The JTAG fast path (SPI and DMA) needs TCK on SPI2 SCK (PB13), TDI on
// SPI2 MOSI (PB15), TDO on SPI2 MISO (PB14), and TMS on the remapped SPI1
// MISO (PB4). SPI1 is a slave and gets its clock from TCK through a board
// link to the remapped SPI1 SCK (PB3).
struct JtagFastPath {
   Pin tckLink;
};

struct Uart {
   uint8_t usart; // USARTn
   Pin tx;
   Pin rx;
};

// Target voltage (Vref) sense on an external ADC channel (0-15):
// Vref = ADC input voltage * divider.
struct VrefSense {
   Pin pin;
   uint8_t adcChannel;
   uint8_t divider;
};

// Target power switch. Drive the pin to the active level to switch on.
// To switch off, the pin floats (input).
struct PowerSwitch {
   Pin pin;
   Active on;
};

struct Leds {
   std::optional<Signal> power;
   std::optional<Signal> state;
   std::optional<Signal> usb;
};

struct Definition {
   std::string_view name;
   uint32_t hseFrequency; // crystal, Hz
   uint8_t hardwareId;    // reply of VERSALOON_GET_HARDWARE
   DebugPins debug;
   std::optional<JtagFastPath> jtagFastPath;
   std::optional<Uart> uart;
   std::optional<VrefSense> vref;
   std::optional<PowerSwitch> powerSwitch;
   Leds leds;
   std::optional<Signal> key;
   // Software-controlled USB D+ pull-up. Empty: the pull-up is fixed.
   std::optional<Signal> usbPullUp;

   [[nodiscard]] constexpr bool hasJtagFastPath() const {
      return jtagFastPath.has_value();
   }
};

// Pins that no board signal can use: USB D- and D+ (PA11, PA12) and the
// crystal (PD0, PD1). The SWD pins of the dongle MCU (PA13, PA14) are not
// in this list, because a release build of the Mini uses PA14 for an LED.
inline constexpr std::array<Pin, 4> reservedPins{ {
{ Port::A, 11 },
{ Port::A, 12 },
{ Port::D, 0 },
{ Port::D, 1 },
} };

namespace detail {

constexpr bool isValidPin(const Pin &pin) {
   return pin.number < 16;
}

// All pins of a definition. Unused optional pins are empty.
constexpr std::array<std::optional<Pin>, 17> allPins(const Definition &def) {
   auto pinOf = [](const auto &opt) -> std::optional<Pin> { return opt ? std::optional<Pin>(opt->pin) : std::nullopt; };
   return { {
   def.debug.tck,
   def.debug.tmsOut, // tmsIn is checked separately: it can be the same pin as tmsOut.
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
// used twice (except TMS out and in), no reserved pin, and the fixed fast
// path pins when the board has the JTAG fast path. Each board header checks
// its definition with a static_assert.
constexpr bool isValid(const Definition &def) {
   const auto pins = detail::allPins(def);
   for(std::size_t i = 0; i < pins.size(); ++i) {
      if(!pins[i]) {
         continue;
      }
      if(!detail::isValidPin(*pins[i])) {
         return false;
      }
      for(const auto &reserved : reservedPins) {
         if(*pins[i] == reserved) {
            return false;
         }
      }
      for(std::size_t j = i + 1; j < pins.size(); ++j) {
         if(pins[j] && *pins[i] == *pins[j]) {
            return false;
         }
      }
      // tmsIn may only share a pin with tmsOut.
      if(*pins[i] == def.debug.tmsIn && *pins[i] != def.debug.tmsOut) {
         return false;
      }
   }
   if(!detail::isValidPin(def.debug.tmsIn)) {
      return false;
   }
   if(def.jtagFastPath) {
      const auto &d = def.debug;
      if(d.tck != Pin{ Port::B, 13 } || d.tdi != Pin{ Port::B, 15 } || d.tdo != Pin{ Port::B, 14 } ||
         d.tmsOut != Pin{ Port::B, 4 } || def.jtagFastPath->tckLink != Pin{ Port::B, 3 }) {
         return false;
      }
   }
   if(def.vref && (def.vref->adcChannel > 15 || def.vref->divider == 0)) {
      return false;
   }
   return def.hseFrequency >= 4'000'000 && def.hseFrequency <= 16'000'000;
}

} // namespace board
