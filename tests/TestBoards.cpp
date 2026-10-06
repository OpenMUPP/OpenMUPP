#include "board/boards/Mini.h"
#include "board/boards/OtherUse.h"
#include "board/boards/StLinkV2.h"

#include <doctest/doctest.h>

using board::Active;
using board::Definition;
using board::Pin;
using board::Port;
using board::Signal;

TEST_CASE("Versaloon Mini board definition has the documented pins") {
   const Definition &mini = board::mini;

   CHECK(mini.name == "mini");
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
   CHECK(board::adcChannelOf(mini.vref->pin) == 8);
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

   const Definition &expected = board::mini;

   CHECK(otherUse.hseFrequency == expected.hseFrequency);
   CHECK(otherUse.hardwareId == expected.hardwareId);
   CHECK(otherUse.debug.tck == expected.debug.tck);
   CHECK(otherUse.debug.tmsOut == expected.debug.tmsOut);
   CHECK(otherUse.debug.tmsIn == expected.debug.tmsIn);
   CHECK(otherUse.debug.tdi == expected.debug.tdi);
   CHECK(otherUse.debug.tdo == expected.debug.tdo);
   CHECK(otherUse.debug.trst == expected.debug.trst);
   CHECK(otherUse.debug.srst == expected.debug.srst);
   CHECK(otherUse.debug.rtck == expected.debug.rtck);
   CHECK(otherUse.hasJtagFastPath());
   CHECK(otherUse.leds.power == expected.leds.power);
   CHECK(otherUse.key == expected.key);
}

TEST_CASE("ST-Link/V2 has an 8 MHz crystal and no JTAG fast path") {
   const Definition &stLink = board::stLinkV2;

   CHECK(stLink.name == "stlinkv2");
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
   CHECK(board::adcChannelOf({ Port::A, 0 }) == 0);
   CHECK(board::adcChannelOf({ Port::A, 7 }) == 7);
   CHECK(board::adcChannelOf({ Port::B, 0 }) == 8);
   CHECK(board::adcChannelOf({ Port::B, 1 }) == 9);
   CHECK(board::adcChannelOf({ Port::C, 0 }) == 10);
   CHECK(board::adcChannelOf({ Port::C, 5 }) == 15);
   CHECK_FALSE(board::adcChannelOf({ Port::A, 8 }));
   CHECK_FALSE(board::adcChannelOf({ Port::B, 2 }));
   CHECK_FALSE(board::adcChannelOf({ Port::C, 6 }));
   CHECK_FALSE(board::adcChannelOf({ Port::D, 0 }));
}

TEST_CASE("isValid checks the crystal and the Vref pin") {
   Definition def = board::mini;

   SUBCASE("crystal too fast") {
      def.hseFrequency = 25'000'000;
   }
   SUBCASE("Vref pin without an ADC channel") {
      def.vref->pin = Pin{ Port::A, 8 };
   }
   SUBCASE("no divider") {
      def.vref->divider = 0;
   }

   CHECK_FALSE(board::isValid(def));
}
