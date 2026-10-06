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

// Target voltage (Vref) sense: Vref = ADC input voltage * divider.
// The pin must have an ADC channel (see adcChannelOf()).
struct VrefSense {
   Pin pin;
   uint8_t divider;
};

// ADC1/ADC2 input channel of a pin on the STM32F103 (RM0008 11.3.1):
// PA0-PA7 are channels 0-7, PB0-PB1 are 8-9 and PC0-PC5 are 10-15.
// Empty when the pin has no ADC input.
constexpr std::optional<uint8_t> adcChannelOf(Pin pin) {
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
   case Port::D:
      break;
   }
   return std::nullopt;
}

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
   // Add each new pin field to detail::allPins() too, else isValid() does
   // not check it.

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
// with an ADC channel, and the fixed fast
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
   }
   if(def.jtagFastPath) {
      const auto &d = def.debug;
      if(d.tck != Pin{ Port::B, 13 } || d.tdi != Pin{ Port::B, 15 } || d.tdo != Pin{ Port::B, 14 } ||
         d.tmsOut != Pin{ Port::B, 4 } || def.jtagFastPath->tckLink != Pin{ Port::B, 3 }) {
         return false;
      }
   }
   if(def.vref && (!adcChannelOf(def.vref->pin) || def.vref->divider == 0)) {
      return false;
   }
   return def.hseFrequency >= 4'000'000 && def.hseFrequency <= 16'000'000;
}

} // namespace board
