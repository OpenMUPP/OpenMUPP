// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/VrefScale.h"
#include "board/boards/Mini.h"
#include "board/boards/OtherUse.h"
#include "board/boards/StLinkV21.h"
#include "board/boards/StLinkV2MiniClone.h"
#include "board/boards/WeActH523.h"
#include "board/boards/YdCh32V307.h"

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
constexpr Family V30x = Family::Ch32V30x;

// An STM32H5 test board with a 24 MHz crystal and the TCK link on PB3. It
// tests the STM32H5 rules apart from the board definitions.
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

TEST_CASE("ST-Link/V2-1 has the pin map of the Nucleo-64 schematic") {
   const Definition &stLink = board::stLinkV21;

   CHECK(stLink.name == "stlinkv21");
   CHECK(stLink.family == F1);
   CHECK(stLink.hseFrequency == 8'000'000);
   CHECK(stLink.hardwareId == 0x38);
   CHECK_FALSE(stLink.hasJtagFastPath());
   CHECK(stLink.debug.tmsOut == Pin{ Port::B, 14 });
   CHECK(stLink.debug.tmsIn == Pin{ Port::B, 12 });
   CHECK(stLink.debug.srst == Pin{ Port::B, 0 });
   CHECK_FALSE(stLink.powerSwitch);
   CHECK(stLink.leds.state == Signal{ { Port::A, 9 }, Active::Low });
   CHECK(stLink.leds.stateTwoColour);
   // USB_RENUMn: high connects the D+ pull-up, low disconnects it
   CHECK(stLink.usbPullUp == Signal{ { Port::A, 15 }, Active::High });
   CHECK(stLink.usbPullUpOpenDrain);
   CHECK(board::swjConfig(stLink) == board::SwjConfig::NoJtag);
   CHECK(board::withSwd(stLink) == stLink);
}

TEST_CASE("ST-Link/V2 mini clone has the measured pin map of the dongle") {
   const Definition &clone = board::stLinkV2MiniClone;

   CHECK(clone.name == "stlinkv2miniclone");
   CHECK(clone.family == F1);
   CHECK(clone.hseFrequency == 8'000'000);
   CHECK(clone.hardwareId == 0x38);
   // Header: CLK, DIO and RST. PB13 and PB5 are tied to them and stay inputs.
   CHECK(clone.debug.tck == Pin{ Port::A, 5 });
   CHECK(clone.debug.tmsOut == Pin{ Port::B, 14 });
   // DIO: PB14 drives, PB12 reads with its pull-up
   CHECK(clone.debug.tmsIn == Pin{ Port::B, 12 });
   CHECK(clone.debug.srst == Pin{ Port::B, 6 });
   CHECK(clone.leds.state == Signal{ { Port::A, 9 }, Active::High });
   CHECK(clone.leds.stateTwoColour);
   CHECK_FALSE(clone.hasJtagFastPath());
   CHECK_FALSE(clone.powerSwitch);
   CHECK_FALSE(clone.usbPullUp);
   CHECK(board::swjConfig(clone) == board::SwjConfig::Full);
   CHECK(board::withSwd(clone) == clone);
}

TEST_CASE("WeAct H523 has SWDIO next to SWCLK and the on-board LED and key") {
   const Definition &weAct = board::weActH523;

   CHECK(weAct.name == "weacth523");
   CHECK(weAct.family == board::Family::Stm32H5);
   CHECK(weAct.hseFrequency == 8'000'000);
   // Header P2 pins 1 to 7: SWDIO, SWCLK, TDO, TDI, RTCK, TX, RX
   CHECK(weAct.debug.tmsIn == Pin{ Port::B, 12 });
   CHECK(weAct.debug.tck == Pin{ Port::B, 13 });
   CHECK(weAct.debug.tdo == Pin{ Port::B, 14 });
   CHECK(weAct.debug.tdi == Pin{ Port::B, 15 });
   CHECK(weAct.debug.rtck == Pin{ Port::A, 8 });
   CHECK(weAct.uart == board::Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } });
   // SPI1 MISO of the fast path, with a wire to PB12
   CHECK(weAct.debug.tmsOut == Pin{ Port::A, 6 });
   CHECK(weAct.debug.trst == Pin{ Port::B, 4 });
   CHECK(weAct.debug.srst == Pin{ Port::B, 5 });
   CHECK(weAct.vref == board::VrefSense{ .pin{ Port::B, 0 }, .divider = 1 });
   CHECK(weAct.leds.state == Signal{ { Port::C, 13 }, Active::Low });
   CHECK_FALSE(weAct.leds.power);
   CHECK_FALSE(weAct.leds.usb);
   CHECK(weAct.key == Signal{ { Port::A, 0 }, Active::Low });
   REQUIRE(weAct.hasJtagFastPath());
   CHECK(weAct.jtagFastPath->tckLink == Pin{ Port::B, 3 });
   CHECK_FALSE(weAct.powerSwitch);
   CHECK_FALSE(weAct.usbPullUp);
   CHECK(board::swjConfig(weAct) == board::SwjConfig::NoJtag);
   CHECK(board::withSwd(weAct) == weAct);
}

TEST_CASE("All boards are valid") {
   CHECK(board::isValid(board::mini));
   CHECK(board::isValid(board::otherUse));
   CHECK(board::isValid(board::stLinkV21));
   CHECK(board::isValid(board::stLinkV2MiniClone));
   CHECK(board::isValid(board::weActH523));
   CHECK(board::isValid(board::ydCh32V307));
}

TEST_CASE("YD-CH32V307VCT6 has the pin map of plan D10") {
   const Definition &yd = board::ydCh32V307;

   CHECK(yd.name == "ydch32v307");
   CHECK(yd.family == V30x);
   CHECK(yd.hseFrequency == 8'000'000);
   CHECK(yd.debug.tck == Pin{ Port::B, 13 });
   CHECK(yd.debug.tdi == Pin{ Port::B, 15 });
   CHECK(yd.debug.tdo == Pin{ Port::B, 14 });
   // One pin is SWDIO out and in
   CHECK(yd.debug.tmsOut == Pin{ Port::C, 11 });
   CHECK(yd.debug.tmsIn == Pin{ Port::C, 11 });
   CHECK(yd.debug.srst == Pin{ Port::B, 12 });
   CHECK(yd.debug.trst == Pin{ Port::B, 9 });
   CHECK(yd.debug.rtck == Pin{ Port::A, 8 });
   CHECK(yd.uart == board::Uart{ .usart = 1, .tx{ Port::A, 9 }, .rx{ Port::A, 10 } });
   CHECK(yd.vref == board::VrefSense{ .pin{ Port::B, 0 }, .divider = 1 });
   CHECK(yd.leds.state == Signal{ { Port::B, 4 }, Active::High });
   CHECK(yd.leds.usb == Signal{ { Port::A, 15 }, Active::High });
   CHECK_FALSE(yd.leds.power);
   CHECK(yd.key == Signal{ { Port::B, 3 }, Active::Low });
   CHECK_FALSE(yd.hasJtagFastPath());
   CHECK_FALSE(yd.powerSwitch);
   CHECK_FALSE(yd.usbPullUp);
}

TEST_CASE("isValid checks the CH32V307 rules") {
   Definition def = board::ydCh32V307;

   SUBCASE("USBHS pin") {
      def.debug.trst = Pin{ Port::B, 6 };
   }
   SUBCASE("crystal pin") {
      def.key = Signal{ { Port::D, 1 }, Active::Low };
   }
   SUBCASE("crystal too slow") {
      def.hseFrequency = 2'000'000;
   }
   SUBCASE("crystal too fast") {
      def.hseFrequency = 26'000'000;
   }
   SUBCASE("no crystal") {
      def.hseFrequency = 0;
   }
   SUBCASE("JTAG fast path (not ported)") {
      def.jtagFastPath = board::JtagFastPath{ .tckLink{ Port::C, 10 } };
   }
   SUBCASE("SWD pin PA13 (no SW_CFG write)") {
      def.leds.usb = Signal{ { Port::A, 13 }, Active::High };
   }
   SUBCASE("SWD pin PA14 (no SW_CFG write)") {
      def.leds.usb = Signal{ { Port::A, 14 }, Active::High };
   }
   SUBCASE("Vref pin without an ADC channel") {
      // PB8 is free on the YD board, so only the ADC rule rejects it.
      def.vref->pin = Pin{ Port::B, 8 };
   }

   CHECK_FALSE(board::isValid(def));
}

TEST_CASE("The CH32V307 has the ADC channels of the STM32F103 and no one-wire timer yet") {
   CHECK(board::adcChannelOf(V30x, { Port::B, 0 }) == 8);
   CHECK(board::adcChannelOf(V30x, { Port::C, 5 }) == 15);
   CHECK_FALSE(board::adcChannelOf(V30x, { Port::B, 2 }));
   CHECK_FALSE(board::oneWireTimerOf(V30x, board::ydCh32V307.debug.tmsOut));
   // PB6 and PB7 are normal pins on the other families: SRST of the mini
   // clone is PB6.
   CHECK(board::stLinkV2MiniClone.debug.srst == Pin{ Port::B, 6 });
   CHECK(board::isValid(board::stLinkV2MiniClone));
   Definition def = board::weActH523;
   def.debug.trst = Pin{ Port::B, 7 };
   CHECK(board::isValid(def));
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
   for(const Definition &def : { board::mini, board::otherUse, board::stLinkV21, board::stLinkV2MiniClone }) {
      CAPTURE(def.name);
      const auto timer = board::oneWireTimerOf(def.family, def.debug.tmsOut);
      REQUIRE(timer);
      CHECK(timer->channel != timer->sampleChannel);
      CHECK(timer->updateDma != timer->sampleDma);
   }
   CHECK(board::oneWireTimerOf(F1, board::mini.debug.tmsOut)->timer == 3);
   CHECK(board::oneWireTimerOf(F1, board::stLinkV21.debug.tmsOut)->complementary);
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

TEST_CASE("The STM32H5 one-wire timer is TIM3_CH1 on PA6") {
   CHECK(board::oneWireTimerOf(F1, { Port::B, 4 }));
   CHECK_FALSE(board::oneWireTimerOf(H5, { Port::B, 4 }));
   const auto timer = board::oneWireTimerOf(H5, h5Board.debug.tmsOut);
   REQUIRE(timer);
   CHECK(timer->timer == 3);
   CHECK(timer->channel == 1);
   CHECK(timer->sampleChannel == 3);
   // The JTAG fast path uses GPDMA1 channels 0 to 2
   CHECK(timer->updateDma >= 3);
   CHECK(timer->sampleDma >= 3);
   CHECK(timer->updateDma != timer->sampleDma);
}

TEST_CASE("The reserved crystal pins depend on the family") {
   Definition def = board::stLinkV21;
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
      // PB12 is free on the Mini, so only the ADC rule rejects it.
      def.vref->pin = Pin{ Port::B, 12 };
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
   // The mini clone uses no debug port pin.
   CHECK(board::swjConfig(board::stLinkV2MiniClone) == board::SwjConfig::Full);

   Definition def = board::mini;
   def.leds.usb.reset();
   // PA15 (JTDI) is still the power LED, and PB3/PB4 are the fast path and TMS.
   CHECK(board::swjConfig(def) == board::SwjConfig::NoJtag);

   def = board::stLinkV2MiniClone;
   def.leds.power = Signal{ { Port::A, 13 }, Active::Low };
   CHECK(board::swjConfig(def) == board::SwjConfig::Disabled);

   def = board::stLinkV2MiniClone;
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

   // The mini clone uses no SWD pin: no change.
   CHECK(board::withSwd(board::stLinkV2MiniClone) == board::stLinkV2MiniClone);

   // An LED on SWDIO (PA13) goes too.
   Definition def = board::stLinkV2MiniClone;
   def.leds.power = Signal{ { Port::A, 13 }, Active::Low };
   CHECK_FALSE(board::withSwd(def).leds.power.has_value());

   // A key on an SWD pin stays, and SWD stays off. board/Board.h refuses
   // such a board in a MUPP_DEBUG_SWD build.
   def = board::stLinkV2MiniClone;
   def.key = Signal{ { Port::A, 14 }, Active::Low };
   CHECK(board::swjConfig(board::withSwd(def)) == board::SwjConfig::Disabled);
}

TEST_CASE("stateDriveOf drives one LED or two colours on one pin") {
   using board::StateDrive;
   using board::StateShow;
   constexpr Signal highOn{ { Port::A, 9 }, Active::High };
   constexpr Signal lowOn{ { Port::A, 15 }, Active::Low };

   // One LED: on for activity and a fault, as before
   CHECK(board::stateDriveOf(highOn, false, StateShow::Idle) == StateDrive{ false, false });
   CHECK(board::stateDriveOf(highOn, false, StateShow::Activity) == StateDrive{ false, true });
   CHECK(board::stateDriveOf(highOn, false, StateShow::Fault) == StateDrive{ false, true });
   CHECK(board::stateDriveOf(lowOn, false, StateShow::Idle) == StateDrive{ false, true });
   CHECK(board::stateDriveOf(lowOn, false, StateShow::Fault) == StateDrive{ false, false });

   // Two colours: idle the asserted colour, a fault the other, activity off
   CHECK(board::stateDriveOf(highOn, true, StateShow::Idle) == StateDrive{ false, true });
   CHECK(board::stateDriveOf(highOn, true, StateShow::Fault) == StateDrive{ false, false });
   CHECK(board::stateDriveOf(highOn, true, StateShow::Activity).off);
   CHECK(board::stateDriveOf(lowOn, true, StateShow::Idle) == StateDrive{ false, false });
}

TEST_CASE("Only the mini clone and the ST-Link/V2-1 have a two-colour state LED") {
   for(const Definition &def : { board::mini, board::otherUse, board::weActH523 }) {
      CAPTURE(def.name);
      CHECK_FALSE(def.leds.stateTwoColour);
   }
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
