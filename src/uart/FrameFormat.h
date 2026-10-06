#pragma once

// Maps the CDC line coding of the host (art::usb::LineCoding) to the frame
// format of the UART. Hardware-independent, so the host tests check every
// line coding.
//
// The supported formats are the formats of the USART of the STM32F1 (USART
// v1, RM0008 section 27). Its frame has 8 or 9 bits, one of them the parity
// bit when parity is on:
//
//   Data bits | Parity      | Stop bits
//   ----------+-------------+-------------
//   8         | none        | 1, 1.5 or 2
//   8         | even or odd | 1, 1.5 or 2
//   7         | even or odd | 1, 1.5 or 2
//
// toFrameFormat() rejects all other line codings: 5, 6 or 16 data bits, 7
// data bits without parity, mark or space parity, and unknown codes. It also
// rejects a baud rate that the baud rate generator cannot make within
// maxBaudErrorPercent (see baudRateDivider()). The UART bridge then refuses
// the SET_LINE_CODING request: the request stalls and the UART keeps its
// old format.

#include <cstdint>
#include <optional>
#include <usb/CdcAcmFunction.h>

namespace uart {

enum class DataBits : uint8_t { Seven, Eight };
enum class Parity : uint8_t { None, Even, Odd };
enum class StopBits : uint8_t { One, OneAndHalf, Two };

struct FrameFormat {
   uint32_t baudRate;
   DataBits dataBits;
   Parity parity;
   StopBits stopBits;

   constexpr bool operator==(const FrameFormat &) const = default;
};

// Largest difference between the requested and the real baud rate. A UART
// receiver with 16x oversampling accepts about 3.75 % for the sum of both
// ends, so each end gets about half of it.
inline constexpr uint32_t maxBaudErrorPercent = 2;

// Divider of the baud rate generator (BRR with 16x oversampling: fCK / baud,
// 4 fraction bits) for a UART clock of uartClock Hz. Empty when the divider
// is out of the range of BRR (16 to 0xFFFF), or when the real baud rate is
// more than maxBaudErrorPercent off. At 72 MHz the range is 1099 to 4.5 M
// baud; above 2.88 M baud only some rates are within the error.
constexpr std::optional<uint32_t> baudRateDivider(uint32_t baudRate, uint32_t uartClock) {
   if(baudRate == 0) {
      return std::nullopt;
   }
   const uint64_t divider = (static_cast<uint64_t>(uartClock) + baudRate / 2) / baudRate;
   if(divider < 16 || divider > 0xFFFF) {
      return std::nullopt;
   }
   // |uartClock / divider - baudRate| <= baudRate * max / 100, without
   // the rounding of the division.
   const uint64_t real = uartClock;
   const uint64_t wanted = divider * baudRate;
   const uint64_t error = real > wanted ? real - wanted : wanted - real;
   if(error * 100 > wanted * maxBaudErrorPercent) {
      return std::nullopt;
   }
   return static_cast<uint32_t>(divider);
}

// The frame format for the line coding, or empty when the UART cannot do it
// (see the table above). uartClock is the clock of the UART in Hz.
constexpr std::optional<FrameFormat> toFrameFormat(const art::usb::LineCoding &coding, uint32_t uartClock) {
   if(!baudRateDivider(coding.dwDTERate, uartClock)) {
      return std::nullopt;
   }

   // PSTN 1.20 table 17: 0 = 1, 1 = 1.5, 2 = 2 stop bits
   StopBits stopBits{};
   switch(coding.bCharFormat) {
   case 0:
      stopBits = StopBits::One;
      break;
   case 1:
      stopBits = StopBits::OneAndHalf;
      break;
   case 2:
      stopBits = StopBits::Two;
      break;
   default:
      return std::nullopt;
   }

   // 0 = none, 1 = odd, 2 = even. Mark (3) and space (4) need a parity bit
   // with a fixed value, which the USART does not have.
   Parity parity{};
   switch(coding.bParityType) {
   case 0:
      parity = Parity::None;
      break;
   case 1:
      parity = Parity::Odd;
      break;
   case 2:
      parity = Parity::Even;
      break;
   default:
      return std::nullopt;
   }

   DataBits dataBits{};
   switch(coding.bDataBits) {
   case 7:
      // The 7 data bits and the parity bit make the 8-bit frame. There is
      // no 7-bit frame.
      if(parity == Parity::None) {
         return std::nullopt;
      }
      dataBits = DataBits::Seven;
      break;
   case 8:
      dataBits = DataBits::Eight;
      break;
   default:
      return std::nullopt;
   }

   return FrameFormat{ coding.dwDTERate, dataBits, parity, stopBits };
}

} // namespace uart
