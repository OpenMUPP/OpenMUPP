#pragma once

// The UART of the target as the CMSIS-DAP command processor (DapProcessor.h)
// sees it: the DAP_UART_* commands of CMSIS-DAP v2.1. Hardware-independent.
// uart/DapUartChannel.h gives the implementation on the UART of the board,
// with the owner switch between the CDC-ACM serial port and the DAP
// commands.

#include <cstddef>
#include <cstdint>
#include <utils/compat/span.h>

namespace cmsisdap {

// The transports of DAP_UART_Transport: the owner of the UART
namespace uartTransport {
inline constexpr uint8_t none = 0;
inline constexpr uint8_t comPort = 1;
inline constexpr uint8_t dap = 2;
} // namespace uartTransport

// The control bits of DAP_UART_Control. A disable bit wins over its enable
// bit.
namespace uartControl {
inline constexpr uint8_t rxEnable = 0x01;
inline constexpr uint8_t rxDisable = 0x02;
inline constexpr uint8_t rxFlush = 0x04;
inline constexpr uint8_t txEnable = 0x10;
inline constexpr uint8_t txDisable = 0x20;
inline constexpr uint8_t txFlush = 0x40;
} // namespace uartControl

// The status bits of DAP_UART_Status and DAP_UART_Transfer. A read of the
// status clears the error bits.
namespace uartStatus {
inline constexpr uint8_t rxEnabled = 0x01;
inline constexpr uint8_t rxDataLost = 0x02;
inline constexpr uint8_t framingError = 0x04;
inline constexpr uint8_t parityError = 0x08;
inline constexpr uint8_t txEnabled = 0x10;
} // namespace uartStatus

// The error bits of DAP_UART_Configure
namespace uartConfigError {
inline constexpr uint8_t dataBits = 0x01;
inline constexpr uint8_t parity = 0x02;
inline constexpr uint8_t stopBits = 0x04;
inline constexpr uint8_t all = dataBits | parity | stopBits;
} // namespace uartConfigError

// The fields of the control byte of DAP_UART_Configure (the values of the
// CMSIS-Driver USART interface, asynchronous mode without flow control)
namespace uartConfig {
// Bits 3..0: 0 or 8 = 8 data bits, 5 to 7 = 5 to 7 data bits
inline constexpr uint8_t dataBitsMask = 0x0F;
// Bits 5..4: 0 = none, 1 = even, 2 = odd
inline constexpr uint8_t parityShift = 4;
// Bits 7..6: 0 = 1, 1 = 2, 2 = 1.5 stop bits
inline constexpr uint8_t stopBitsShift = 6;
} // namespace uartConfig

class DapUart {
   public:
   // True when the UART also has the CDC-ACM serial port as a transport
   // (DAP_Info capabilities, second byte bit 0).
   [[nodiscard]] virtual bool hasComPort() const = 0;
   // The sizes of the receive and the transmit buffer (DAP_Info 0xFB, 0xFC)
   [[nodiscard]] virtual uint32_t rxBufferSize() const = 0;
   [[nodiscard]] virtual uint32_t txBufferSize() const = 0;

   // DAP_UART_Transport. True for DAP_OK. A transport that is not known or
   // not available gives false, and the owner stays.
   [[nodiscard]] virtual bool setTransport(uint8_t transport) = 0;
   // DAP_UART_Configure. Returns the error bits (uartConfigError).
   // baudRate becomes the real baud rate, or 0 for a baud rate error.
   [[nodiscard]] virtual uint8_t configure(uint8_t control, uint32_t &baudRate) = 0;
   // DAP_UART_Control (uartControl bits). True for DAP_OK.
   [[nodiscard]] virtual bool control(uint8_t control) = 0;
   // The status bits (uartStatus). Clears the error bits.
   [[nodiscard]] virtual uint8_t status() = 0;
   // The bytes in the receive buffer, and the bytes in the transmit buffer
   // that are not sent yet
   [[nodiscard]] virtual uint32_t rxCount() const = 0;
   [[nodiscard]] virtual uint32_t txCount() const = 0;
   // DAP_UART_Transfer: puts the bytes that fit into the transmit buffer,
   // and takes up to data.size() received bytes. Both return the count.
   [[nodiscard]] virtual std::size_t write(art::span<const uint8_t> data) = 0;
   [[nodiscard]] virtual std::size_t read(art::span<uint8_t> data) = 0;

   // The host went away: the UART goes back to its default transport.
   virtual void hostGone() = 0;

   protected:
   ~DapUart() = default;
};

} // namespace cmsisdap
