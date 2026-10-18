#include "board/VrefScale.h"
#include "board/boards/Mini.h"
#include "board/boards/OtherUse.h"
#include "board/boards/StLinkV2.h"

#include <doctest/doctest.h>

using board::Active;
using board::Definition;
using board::Family;
using board::Pin;
using board::Port;
using board::Signal;

namespace {

constexpr Family F1 = Family::Stm32F1;
constexpr Family H5 = Family::Stm32H5;

// An STM32H5 board with the pin map of the NUCLEO-H533RE port (#89). It
// tests the STM32H5 rules until the first STM32H5 board is in
// src/board/boards/.
constexpr Definition h5Board{
   .name = "h5-test",
   .hseFrequency = 24'000'000,
   .family = H5,
   .hardwareId = 0,
   .debug{
   .tck{ Port::B, 13 },
   .tmsOut{ Port::A, 6 },
   .tmsIn{ Port::A, 7 },
   .tdi{ Port::B, 15 },
   .tdo{ Port::B, 14 },
   .trst = Pin{ Port::B, 4 },
   .srst{ Port::B, 10 },
   .rtck = Pin{ Port::A, 0 },
   },
   .jtagFastPath = board::JtagFastPath{ .tckLink{ Port::B, 3 } },
   .uart = board::Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } },
   .vref = board::VrefSense{ .pin{ Port::B, 0 }, .divider = 2 },
   .powerSwitch = std::nullopt,
   .leds{
   .power = std::nullopt,
   .state = Signal{ { Port::A, 5 }, Active::High },
   .usb = std::nullopt,
   },
   .key = Signal{ { Port::C, 13 }, Active::High },
   .usbPullUp = std::nullopt,
};
static_assert(board::isValid(h5Board));

} // namespace

TEST_CASE("Versaloon Mini board definition has the documented pins") {
   const Definition &mini = board::mini;

   CHECK(mini.name == "mini");
   CHECK(mini.family == F1);
   CHECK(mini.hseFrequency == 12'000'000);
   CHECK(mini.hardwareId == 0x15);

   CHECK(mini.debug.tck == Pin{ Port::B, 13 });
   CHECK(mini.debug.tdi == Pin{ Port::B, 15 });
   CHECK(mini.debug.tdo == Pin{ Port::B, 14 });
   CHECK(mini.debug.tmsOut == Pin{ Port::B, 4 });
   CHECK(mini.debug.tmsIn == Pin{ Port::B, 6 });
   CHECK(mini.debug.trst == Pin{ Port::B, 10 });
   CHECK(mini.debug.srst == Pin{ Port::B, 11 });
   CHECK(mini.debug.rtck == Pin{ Port::A, 8 });

   REQUIRE(mini.hasJtagFastPath());
   CHECK(mini.jtagFastPath->tckLink == Pin{ Port::B, 3 });

   REQUIRE(mini.uart);
   CHECK(mini.uart->usart == 1);
   CHECK(mini.uart->tx == Pin{ Port::A, 9 });
   CHECK(mini.uart->rx == Pin{ Port::A, 10 });

   REQUIRE(mini.vref);
   CHECK(mini.vref->pin == Pin{ Port::B, 0 });
   CHECK(board::adcChannelOf(mini.family, mini.vref->pin) == 8);
   CHECK(mini.vref->divider == 2);

   REQUIRE(mini.powerSwitch);
   CHECK(mini.powerSwitch->pin == Pin{ Port::B, 8 });
   CHECK(mini.powerSwitch->on == Active::Low);

   CHECK(mini.leds.power == Signal{ { Port::A, 15 }, Active::Low });
   CHECK(mini.leds.state == Signal{ { Port::B, 5 }, Active::Low });
   CHECK(mini.leds.usb == Signal{ { Port::A, 14 }, Active::Low });
   CHECK(mini.key == Signal{ { Port::B, 9 }, Active::Low });
   CHECK(mini.usbPullUp == Signal{ { Port::C, 13 }, Active::High });
}

TEST_CASE("OtherUse differs from the Mini only in the name and the USB pull-up") {
   const Definition &otherUse = board::otherUse;

   CHECK(otherUse.name == "otheruse");
   CHECK(otherUse.usbPullUp == Signal{ { Port::A, 2 }, Active::High });

   Definition expected = board::mini;
   expected.name = "otheruse";
   expected.usbPullUp = Signal{ { Port::A, 2 }, Active::High };
   CHECK(otherUse == expected);
}

TEST_CASE("ST-Link/V2 has an 8 MHz crystal and no JTAG fast path") {
   const Definition &stLink = board::stLinkV2;

   CHECK(stLink.name == "stlinkv2");
   CHECK(stLink.family == F1);
   CHECK(stLink.hseFrequency == 8'000'000);
   CHECK(stLink.hardwareId == 0x38);
   CHECK_FALSE(stLink.hasJtagFastPath());
   CHECK(stLink.debug.tmsOut == stLink.debug.tmsIn);
   CHECK_FALSE(stLink.powerSwitch);
   CHECK_FALSE(stLink.usbPullUp);
}

TEST_CASE("All boards are valid") {
   CHECK(board::isValid(board::mini));
   CHECK(board::isValid(board::otherUse));
   CHECK(board::isValid(board::stLinkV2));
}

TEST_CASE("isValid rejects pin conflicts") {
   Definition def = board::mini;

   SUBCASE("pin used twice") {
      def.leds.state = Signal{ def.debug.srst, Active::Low };
   }
   SUBCASE("TMS input on another signal") {
      def.debug.tmsIn = def.debug.tdo;
   }
   SUBCASE("TMS input on a USB data pin") {
      def.debug.tmsIn = Pin{ Port::A, 11 };
   }
   SUBCASE("USB data pin") {
      def.key = Signal{ { Port::A, 12 }, Active::Low };
   }
   SUBCASE("pin number out of range") {
      def.debug.trst = Pin{ Port::B, 16 };
   }
   SUBCASE("TMS input pin number out of range") {
      def.debug.tmsIn = Pin{ Port::B, 16 };
   }

   CHECK_FALSE(board::isValid(def));
}

TEST_CASE("isValid checks the JTAG fast path pins") {
   Definition def = board::mini;

   SUBCASE("TCK not on SPI2 SCK") {
      def.debug.tck = Pin{ Port::A, 5 };
   }
   SUBCASE("TCK link not on SPI1 SCK") {
      def.jtagFastPath->tckLink = Pin{ Port::A, 5 };
   }

   CHECK_FALSE(board::isValid(def));

   def.jtagFastPath.reset();
   CHECK(board::isValid(def));
}

TEST_CASE("adcChannelOf gives the STM32F103 ADC channel of a pin") {
   CHECK(board::adcChannelOf(F1, { Port::A, 0 }) == 0);
   CHECK(board::adcChannelOf(F1, { Port::A, 7 }) == 7);
   CHECK(board::adcChannelOf(F1, { Port::B, 0 }) == 8);
   CHECK(board::adcChannelOf(F1, { Port::B, 1 }) == 9);
   CHECK(board::adcChannelOf(F1, { Port::C, 0 }) == 10);
   CHECK(board::adcChannelOf(F1, { Port::C, 5 }) == 15);
   CHECK_FALSE(board::adcChannelOf(F1, { Port::A, 8 }));
   CHECK_FALSE(board::adcChannelOf(F1, { Port::B, 2 }));
   CHECK_FALSE(board::adcChannelOf(F1, { Port::C, 6 }));
   CHECK_FALSE(board::adcChannelOf(F1, { Port::D, 0 }));
}

TEST_CASE("oneWireTimerOf gives a timer channel on the SWDIO output of each board") {
   for(const Definition &def : { board::mini, board::otherUse, board::stLinkV2 }) {
      CAPTURE(def.name);
      const auto timer = board::oneWireTimerOf(def.family, def.debug.tmsOut);
      REQUIRE(timer);
      CHECK(timer->channel != timer->sampleChannel);
      CHECK(timer->updateDma != timer->sampleDma);
   }
   CHECK(board::oneWireTimerOf(F1, board::mini.debug.tmsOut)->timer == 3);
   CHECK(board::oneWireTimerOf(F1, board::stLinkV2.debug.tmsOut)->complementary);
   CHECK_FALSE(board::oneWireTimerOf(F1, { Port::A, 0 }));
}

TEST_CASE("adcChannelOf gives the STM32H5 ADC channel of a pin") {
   // DS14539 table 14: all ADC pins of the 64-pin package
   struct Expected {
      Pin pin;
      uint8_t channel;
   };
   constexpr Expected table[] = {
      { { Port::A, 0 }, 0 },  { { Port::A, 1 }, 1 },  { { Port::A, 2 }, 14 }, { { Port::A, 3 }, 15 },
      { { Port::A, 4 }, 18 }, { { Port::A, 5 }, 19 }, { { Port::A, 6 }, 3 },  { { Port::A, 7 }, 7 },
      { { Port::B, 0 }, 9 },  { { Port::B, 1 }, 5 },  { { Port::C, 0 }, 10 }, { { Port::C, 1 }, 11 },
      { { Port::C, 2 }, 12 }, { { Port::C, 3 }, 13 }, { { Port::C, 4 }, 4 },  { { Port::C, 5 }, 8 },
   };
   for(const auto &e : table) {
      CAPTURE(static_cast<int>(e.pin.port));
      CAPTURE(static_cast<int>(e.pin.number));
      CHECK(board::adcChannelOf(H5, e.pin) == e.channel);
   }
   CHECK_FALSE(board::adcChannelOf(H5, { Port::A, 8 }));
   CHECK_FALSE(board::adcChannelOf(H5, { Port::B, 2 }));
   CHECK_FALSE(board::adcChannelOf(H5, { Port::C, 6 }));
   CHECK_FALSE(board::adcChannelOf(H5, { Port::H, 0 }));
}

TEST_CASE("The STM32H5 has no one-wire timer yet") {
   CHECK(board::oneWireTimerOf(F1, { Port::B, 4 }));
   CHECK_FALSE(board::oneWireTimerOf(H5, { Port::B, 4 }));
   CHECK_FALSE(board::oneWireTimerOf(H5, h5Board.debug.tmsOut));
}

TEST_CASE("The reserved crystal pins depend on the family") {
   Definition def = board::stLinkV2;
   def.key = Signal{ { Port::D, 0 }, Active::Low };
   CHECK_FALSE(board::isValid(def));

   def = h5Board;
   def.key = Signal{ { Port::H, 1 }, Active::Low };
   CHECK_FALSE(board::isValid(def));
   // PD0 is a normal pin on the STM32H5
   def.key = Signal{ { Port::D, 0 }, Active::Low };
   CHECK(board::isValid(def));
}

TEST_CASE("isValid checks the STM32H5 rules") {
   CHECK(board::isValid(h5Board));
   Definition def = h5Board;

   SUBCASE("JTAG fast path TMS on SPI1 MISO of the STM32H5 (PA6)") {
      def.debug.tmsOut = Pin{ Port::B, 4 };
      def.debug.trst = Pin{ Port::A, 6 };
      CHECK_FALSE(board::isValid(def));
      def.jtagFastPath.reset();
      CHECK(board::isValid(def));
   }
   SUBCASE("crystal up to 50 MHz") {
      def.hseFrequency = 50'000'000;
      CHECK(board::isValid(def));
      def.hseFrequency = 50'000'001;
      CHECK_FALSE(board::isValid(def));
   }
   SUBCASE("no crystal") {
      def.hseFrequency = 0;
      CHECK(board::isValid(def));
   }
   SUBCASE("Vref pin with an ADC channel") {
      // PC4 is ADC channel 4, PA8 has no ADC input.
      def.vref->pin = Pin{ Port::C, 4 };
      CHECK(board::isValid(def));
      def.vref->pin = Pin{ Port::A, 8 };
      CHECK_FALSE(board::isValid(def));
   }
}

TEST_CASE("swjConfig is the same on the STM32H5") {
   // PB3 (TCK link) and PB4 (TRST) are JTAG-DP pins: SWD stays on.
   CHECK(board::swjConfig(h5Board) == board::SwjConfig::NoJtag);
   CHECK(board::withSwd(h5Board) == h5Board);
}

TEST_CASE("isValid checks the crystal and the Vref pin") {
   Definition def = board::mini;

   SUBCASE("crystal too fast") {
      def.hseFrequency = 25'000'000;
   }
   SUBCASE("no crystal on an STM32F103") {
      def.hseFrequency = 0;
   }
   SUBCASE("Vref pin without an ADC channel") {
      def.vref->pin = Pin{ Port::A, 8 };
   }
   SUBCASE("no divider") {
      def.vref->divider = 0;
   }

   CHECK_FALSE(board::isValid(def));
}

TEST_CASE("swjConfig frees the debug port pins that the board uses") {
   // PA14 (SWCLK) is the USB LED of the Mini and the OtherUse.
   CHECK(board::swjConfig(board::mini) == board::SwjConfig::Disabled);
   CHECK(board::swjConfig(board::otherUse) == board::SwjConfig::Disabled);
   // The ST-Link/V2 uses no debug port pin.
   CHECK(board::swjConfig(board::stLinkV2) == board::SwjConfig::Full);

   Definition def = board::mini;
   def.leds.usb.reset();
   // PA15 (JTDI) is still the power LED, and PB3/PB4 are the fast path and TMS.
   CHECK(board::swjConfig(def) == board::SwjConfig::NoJtag);

   def = board::stLinkV2;
   def.leds.power = Signal{ { Port::A, 13 }, Active::Low };
   CHECK(board::swjConfig(def) == board::SwjConfig::Disabled);

   def = board::stLinkV2;
   def.key = Signal{ { Port::B, 3 }, Active::Low };
   CHECK(board::swjConfig(def) == board::SwjConfig::NoJtag);
}

TEST_CASE("withSwd drops the LEDs on the SWD pins and keeps SWD on") {
   // Mini and OtherUse: PA14 is the USB LED. PA15 (power LED), PB3 and PB4
   // stay, so the JTAG-DP is off and the SW-DP is on.
   for(const Definition &board : { board::mini, board::otherUse }) {
      const Definition debug = board::withSwd(board);
      CHECK_FALSE(debug.leds.usb.has_value());
      CHECK(debug.leds.power == board.leds.power);
      CHECK(debug.leds.state == board.leds.state);
      CHECK(board::isValid(debug));
      CHECK(board::swjConfig(debug) == board::SwjConfig::NoJtag);

      // All other fields stay the same
      Definition expected = board;
      expected.leds.usb.reset();
      CHECK(debug == expected);
   }

   // The ST-Link/V2 uses no SWD pin: no change.
   CHECK(board::withSwd(board::stLinkV2) == board::stLinkV2);

   // An LED on SWDIO (PA13) goes too.
   Definition def = board::stLinkV2;
   def.leds.power = Signal{ { Port::A, 13 }, Active::Low };
   CHECK_FALSE(board::withSwd(def).leds.power.has_value());

   // A key on an SWD pin stays, and SWD stays off. board/Board.h refuses
   // such a board in a VSL_DEBUG_SWD build.
   def = board::stLinkV2;
   def.key = Signal{ { Port::A, 14 }, Active::Low };
   CHECK(board::swjConfig(board::withSwd(def)) == board::SwjConfig::Disabled);
}

TEST_CASE("Signal::levelFor gives the pin level of the signal state") {
   constexpr Signal activeLow{ { Port::A, 15 }, Active::Low };
   constexpr Signal activeHigh{ { Port::C, 13 }, Active::High };

   CHECK(activeLow.levelFor(true) == false);
   CHECK(activeLow.levelFor(false) == true);
   CHECK(activeHigh.levelFor(true) == true);
   CHECK(activeHigh.levelFor(false) == false);
}

TEST_CASE("The Vref scale gives the target voltage from the ADC reading") {
   // Divider 2 and VDDA 3.3 V: 1.65 V at the pin is 3.3 V at the target.
   CHECK(board::vrefMillivolts(0, 2) == 0);
   CHECK(board::vrefMillivolts(2048, 2) == 3301);
   CHECK(board::vrefMillivolts(2047, 2) == 3299);
   CHECK(board::vrefMillivolts(4095, 2) == 6600);
   // The threshold of the power switch (1.8 V) is 1117 LSB
   CHECK(board::vrefMillivolts(1116, 2) == 1799);
   CHECK(board::vrefMillivolts(1117, 2) == 1800);
   CHECK(board::vrefMillivolts(4095, 1) == 3300);
   CHECK(board::vrefMillivolts(4095, 255) == 0xFFFF);
}
