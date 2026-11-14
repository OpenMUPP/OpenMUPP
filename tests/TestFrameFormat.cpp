// SPDX-License-Identifier: GPL-3.0-or-later

#include "uart/FrameFormat.h"

#include <array>
#include <cstdint>
#include <doctest/doctest.h>
#include <optional>
#include <vector>

namespace {

using uart::DataBits;
using uart::FrameFormat;
using uart::Parity;
using uart::StopBits;

// USART1 on APB2, USART2 on APB1 (36 MHz) of the 72 MHz clock setup
constexpr uint32_t apb2Clock = 72'000'000;
constexpr uint32_t apb1Clock = 36'000'000;

art::usb::LineCoding coding(uint32_t baud, uint8_t dataBits, uint8_t parity, uint8_t stopBits) {
   art::usb::LineCoding result;
   result.dwDTERate = baud;
   result.bDataBits = dataBits;
   result.bParityType = parity;
   result.bCharFormat = stopBits;
   return result;
}

// The expected result, written as the table in uart/FrameFormat.h.
std::optional<FrameFormat> expected(uint32_t baud, uint8_t dataBits, uint8_t parity, uint8_t stopBits) {
   const std::array<std::optional<StopBits>, 3> stops{ StopBits::One, StopBits::OneAndHalf, StopBits::Two };
   const std::array<std::optional<Parity>, 5> parities{ Parity::None, Parity::Odd, Parity::Even, std::nullopt, std::nullopt };
   if(stopBits >= stops.size() || parity >= parities.size() || !parities[parity]) {
      return std::nullopt;
   }
   if(dataBits == 8) {
      return FrameFormat{ baud, DataBits::Eight, *parities[parity], *stops[stopBits] };
   }
   if(dataBits == 7 && parities[parity] != Parity::None) {
      return FrameFormat{ baud, DataBits::Seven, *parities[parity], *stops[stopBits] };
   }
   return std::nullopt;
}

} // namespace

TEST_CASE("toFrameFormat maps every data bits, parity and stop bits combination") {
   // All codes of PSTN 1.20 table 17, and some codes outside of it
   std::vector<uint8_t> dataBitsCodes{ 0, 1, 4, 5, 6, 7, 8, 9, 15, 16, 17, 255 };
   std::vector<uint8_t> parityCodes{ 0, 1, 2, 3, 4, 5, 255 };
   std::vector<uint8_t> stopBitsCodes{ 0, 1, 2, 3, 255 };
   int accepted = 0;
   for(const uint8_t dataBits : dataBitsCodes) {
      for(const uint8_t parity : parityCodes) {
         for(const uint8_t stopBits : stopBitsCodes) {
            CAPTURE(dataBits);
            CAPTURE(parity);
            CAPTURE(stopBits);
            const auto result = uart::toFrameFormat(coding(115200, dataBits, parity, stopBits), apb2Clock);
            CHECK(result == expected(115200, dataBits, parity, stopBits));
            accepted += result ? 1 : 0;
         }
      }
   }
   // 8N, 8O, 8E, 7O, 7E with 3 stop bit settings each
   CHECK(accepted == 15);
}

TEST_CASE("toFrameFormat maps the common line codings") {
   CHECK(uart::toFrameFormat(coding(115200, 8, 0, 0), apb2Clock) ==
         FrameFormat{ 115200, DataBits::Eight, Parity::None, StopBits::One });
   CHECK(uart::toFrameFormat(coding(9600, 7, 2, 0), apb2Clock) == FrameFormat{ 9600, DataBits::Seven, Parity::Even, StopBits::One });
   CHECK(uart::toFrameFormat(coding(19200, 8, 1, 2), apb2Clock) == FrameFormat{ 19200, DataBits::Eight, Parity::Odd, StopBits::Two });
   CHECK(uart::toFrameFormat(coding(57600, 8, 2, 1), apb2Clock) ==
         FrameFormat{ 57600, DataBits::Eight, Parity::Even, StopBits::OneAndHalf });
}

TEST_CASE("toFrameFormat rejects the formats that the USART does not have") {
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 7, 0, 0), apb2Clock)); // 7N: no 7-bit frame
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 5, 0, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 6, 2, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 16, 0, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 8, 3, 0), apb2Clock)); // mark
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 8, 4, 0), apb2Clock)); // space
   CHECK_FALSE(uart::toFrameFormat(coding(115200, 8, 0, 3), apb2Clock));
}

TEST_CASE("toFrameFormat accepts the standard baud rates in the range of the baud rate generator") {
   for(const uint32_t baud : { 1200U, 2400U, 4800U, 9600U, 14400U, 19200U, 38400U, 57600U, 115200U, 230400U, 460800U,
                               500000U, 921600U, 1000000U, 1500000U, 2000000U, 3000000U, 4000000U, 4500000U }) {
      CAPTURE(baud);
      const auto result = uart::toFrameFormat(coding(baud, 8, 0, 0), apb2Clock);
      REQUIRE(result);
      CHECK(result->baudRate == baud);
   }
}

TEST_CASE("toFrameFormat rejects a baud rate out of the range of the baud rate generator") {
   // 0 is not a rate
   CHECK_FALSE(uart::toFrameFormat(coding(0, 8, 0, 0), apb2Clock));
   // Divider above 0xFFFF: the lowest rate at 72 MHz is 1099 baud
   CHECK_FALSE(uart::toFrameFormat(coding(300, 8, 0, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(1098, 8, 0, 0), apb2Clock));
   CHECK(uart::toFrameFormat(coding(1099, 8, 0, 0), apb2Clock));
   // Divider below 16: the highest rate at 72 MHz is 4.5 M baud
   CHECK_FALSE(uart::toFrameFormat(coding(4'600'000, 8, 0, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(10'000'000, 8, 0, 0), apb2Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(UINT32_MAX, 8, 0, 0), apb2Clock));
   // The range follows the clock of the UART
   CHECK(uart::toFrameFormat(coding(2'250'000, 8, 0, 0), apb1Clock));
   CHECK_FALSE(uart::toFrameFormat(coding(3'000'000, 8, 0, 0), apb1Clock));
   CHECK(uart::toFrameFormat(coding(600, 8, 0, 0), apb1Clock));
}

TEST_CASE("baudRateDivider rejects a rate with an error above maxBaudErrorPercent") {
   // 72 MHz / 3.5 M = 20.57: divider 21 gives 3.43 M baud, 2.04 % off
   CHECK_FALSE(uart::baudRateDivider(3'500'000, apb2Clock));
   // 72 MHz / 3.6 M = 20: exact
   CHECK(uart::baudRateDivider(3'600'000, apb2Clock) == 20U);
   // 72 MHz / 921600 = 78.125: divider 78 gives 0.16 % error
   CHECK(uart::baudRateDivider(921'600, apb2Clock) == 78U);
   CHECK(uart::baudRateDivider(115'200, apb2Clock) == 625U);
   // 72 MHz / 1099 = 65514.1
   CHECK(uart::baudRateDivider(1099, apb2Clock) == 65514U);
}

TEST_CASE("baudRateDivider<true> checks the divider of the CH32V307 Vref measurement") {
   constexpr uint32_t ch32Apb2Clock = 144'000'000;
   // Valid at 144 MHz and with (divider + 1) / 2 at 72 MHz
   CHECK(uart::baudRateDivider<true>(115'200, ch32Apb2Clock) == 1250U);
   CHECK(uart::baudRateDivider<true>(3'000'000, ch32Apb2Clock) == 48U);
   CHECK(uart::baudRateDivider<true>(2'198, ch32Apb2Clock) == 65514U);
   // Divider above 0xFFFF at 144 MHz, although 72 MHz alone accepts it
   CHECK_FALSE(uart::baudRateDivider<true>(1'200, ch32Apb2Clock));
   CHECK(uart::baudRateDivider<true>(1'200, apb2Clock) == 60000U);
   // 6 M baud: divider 24 at 144 MHz, but 12 at 72 MHz is below 16
   CHECK(uart::baudRateDivider<false>(6'000'000, ch32Apb2Clock) == 24U);
   CHECK_FALSE(uart::baudRateDivider<true>(6'000'000, ch32Apb2Clock));
   // 3.2 M baud: divider 45 (0 % at 144 MHz), but 23 at 72 MHz gives 3.13 M baud (2.2 %)
   CHECK(uart::baudRateDivider<false>(3'200'000, ch32Apb2Clock) == 45U);
   CHECK_FALSE(uart::baudRateDivider<true>(3'200'000, ch32Apb2Clock));
   // The default follows the build: the host build has no CH32V307
   CHECK(uart::baudRateDivider(6'000'000, ch32Apb2Clock) == 24U);
}
