#pragma once

// The UART of the board for the DAP_UART_* commands of CMSIS-DAP v2.1
// (protocol/cmsisdap/DapUart.h), and the owner switch between these
// commands and the CDC-ACM serial port (uart/UartBridge.h).
// Hardware-independent: the UART is a template parameter, as in the
// bridge, so the host tests run the channel with a fake UART.
//
// Owner: DAP_UART_Transport selects the side that has the UART:
//   1 USB COM port  the bridge (the default when the channel has a bridge)
//   2 DAP commands  DAP_UART_Configure, _Control, _Status and _Transfer
//   0 none          nobody (the default without a bridge)
// Only the owner moves data. The other side gets no data: the bridge
// discards the data of the host, and the DAP commands send and receive
// nothing. A switch stops the TX interrupt and empties both rings, so no
// byte of one side goes to the other side. The byte in the shift register
// still goes out.
//
// The DAP side uses the rings of the bridge (UartBuffers), so DAP_Info
// reports their capacity as the buffer sizes. When the DAP commands get
// the UART, it is not configured, and the receiver and the transmitter
// are disabled:
// - DAP_UART_Configure maps its control byte to a FrameFormat
//   (fromDapControl()), with the formats of the table in
//   uart/FrameFormat.h. The UART gets the new format when the TX path is
//   empty, as in the bridge. Until then, write() takes no bytes. The bytes
//   that the UART received in the old format are discarded. A configuration that fails leaves the UART
//   not configured, and disables the receiver and the transmitter.
// - DAP_UART_Control enables the receiver or the transmitter only when
//   the UART is configured. An enable of the receiver empties the rx ring.
//   A disable or a flush of the transmitter drops the bytes that are not
//   sent yet.
// - While the receiver is disabled, poll() discards the received bytes.
//   The interrupt still receives them.
// - The status bits: data lost for the bytes that the rx ring dropped and
//   the overruns, parity error for the parity errors, framing error for
//   the other receive errors (framing and noise). Each bit tells of the
//   errors since the last read of the status.
//
// A switch away from the DAP commands forgets their configuration. When
// the host goes away (hostGone()), the UART goes back to the default
// owner.
//
// Threading: all functions run in the main loop.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utils/compat/span.h>

#include "protocol/cmsisdap/DapUart.h"
#include "uart/FrameFormat.h"
#include "uart/UartBridge.h"

namespace uart {

// The result of the DAP_UART_Configure control byte and baud rate
struct DapConfiguration {
   // Empty for an error
   std::optional<FrameFormat> format;
   // cmsisdap::uartConfigError bits
   uint8_t errors;
   // The real baud rate, or 0 for an error
   uint32_t baudRate;
};

// Maps the control byte and the baud rate of DAP_UART_Configure to a frame
// format of the UART (the table of uart/FrameFormat.h). uartClock is the
// clock of the UART in Hz. A baud rate that the UART cannot make gives no
// error bit and a baud rate of 0, as in the reference implementation.
constexpr DapConfiguration fromDapControl(uint8_t control, uint32_t baudRate, uint32_t uartClock) {
   namespace error = cmsisdap::uartConfigError;
   uint8_t errors = 0;

   DataBits dataBits = DataBits::Eight;
   switch(control & cmsisdap::uartConfig::dataBitsMask) {
   case 0:
   case 8:
      break;
   case 7:
      dataBits = DataBits::Seven;
      break;
   default:
      errors |= error::dataBits;
      break;
   }

   Parity parity = Parity::None;
   switch((control >> cmsisdap::uartConfig::parityShift) & 0x03u) {
   case 0:
      break;
   case 1:
      parity = Parity::Even;
      break;
   case 2:
      parity = Parity::Odd;
      break;
   default:
      errors |= error::parity;
      break;
   }

   StopBits stopBits = StopBits::One;
   switch((control >> cmsisdap::uartConfig::stopBitsShift) & 0x03u) {
   case 0:
      break;
   case 1:
      stopBits = StopBits::Two;
      break;
   case 2:
      stopBits = StopBits::OneAndHalf;
      break;
   default:
      // 0.5 stop bits
      errors |= error::stopBits;
      break;
   }

   // 7 data bits need the parity bit for the 8-bit frame.
   if(dataBits == DataBits::Seven && parity == Parity::None && (errors & error::parity) == 0) {
      errors |= error::dataBits;
   }

   const auto divider = baudRateDivider(baudRate, uartClock);
   if(errors != 0 || !divider) {
      return { std::nullopt, errors, 0 };
   }
   const uint32_t realBaudRate = (uartClock + *divider / 2) / *divider;
   return { FrameFormat{ baudRate, dataBits, parity, stopBits }, 0, realBaudRate };
}

// Uart is the UART of UartBridge (uart/UartBridge.h), and:
//   void stopTx();   TX interrupt off: the interrupt takes no more bytes
template <typename Uart, std::size_t Size>
class DapUartChannel final : public cmsisdap::DapUart {
   public:
   using Bridge = UartBridge<Uart, Size>;

   // The UART has no owner until start().
   DapUartChannel(Uart &uart, UartBuffers<Size> &buffers)
      : uart_(uart)
      , buffers_(buffers) {
   }

   DapUartChannel(const DapUartChannel &) = delete;
   DapUartChannel &operator=(const DapUartChannel &) = delete;

   // Gives the UART to the default owner: the bridge, or nobody when bridge
   // is nullptr. Call it once, after Bridge::start() and before the USB
   // device starts.
   void start(Bridge *bridge) {
      bridge_ = bridge;
      transport_ = defaultTransport();
      if(bridge_ != nullptr) {
         bridge_->setActive(true);
      }
   }

   // Applies a new format, and discards what nobody takes. Call it in each
   // main loop iteration, with the poll of the bridge.
   void poll() {
      if(transport_ == cmsisdap::uartTransport::none) {
         buffers_.flushRx();
         return;
      }
      if(transport_ != cmsisdap::uartTransport::dap) {
         return;
      }
      if(pending_ && buffers_.tx.empty() && uart_.isTxIdle()) {
         uart_.setFormat(*pending_);
         pending_.reset();
         buffers_.flushRx();
      }
      if(!rxEnabled_) {
         buffers_.flushRx();
      }
   }

   [[nodiscard]] uint8_t transport() const {
      return transport_;
   }

   // cmsisdap::DapUart
   [[nodiscard]] bool hasComPort() const override {
      return bridge_ != nullptr;
   }

   [[nodiscard]] uint32_t rxBufferSize() const override {
      return static_cast<uint32_t>(buffers_.rx.capacity());
   }

   [[nodiscard]] uint32_t txBufferSize() const override {
      return static_cast<uint32_t>(buffers_.tx.capacity());
   }

   [[nodiscard]] bool setTransport(uint8_t transport) override {
      namespace t = cmsisdap::uartTransport;
      if(transport > t::dap || (transport == t::comPort && bridge_ == nullptr)) {
         return false;
      }
      if(transport == transport_) {
         return true;
      }
      uart_.stopTx();
      buffers_.flushTx();
      buffers_.flushRx();
      if(transport_ == t::comPort) {
         bridge_->setActive(false);
      }
      transport_ = transport;
      resetDap();
      if(transport_ == t::comPort) {
         bridge_->setActive(true);
      }
      return true;
   }

   [[nodiscard]] uint8_t configure(uint8_t control, uint32_t &baudRate) override {
      if(transport_ != cmsisdap::uartTransport::dap) {
         baudRate = 0;
         return cmsisdap::uartConfigError::all;
      }
      const DapConfiguration result = fromDapControl(control, baudRate, uart_.clockFrequency());
      baudRate = result.baudRate;
      if(!result.format) {
         configured_ = false;
         rxEnabled_ = false;
         disableTx();
         pending_.reset();
         return result.errors;
      }
      configured_ = true;
      pending_ = result.format;
      return 0;
   }

   [[nodiscard]] bool control(uint8_t control) override {
      namespace c = cmsisdap::uartControl;
      if(transport_ != cmsisdap::uartTransport::dap) {
         return false;
      }
      bool ok = true;
      if((control & c::rxDisable) != 0) {
         rxEnabled_ = false;
      } else if((control & c::rxEnable) != 0) {
         if(!configured_) {
            ok = false;
         } else if(!rxEnabled_) {
            buffers_.flushRx();
            rxEnabled_ = true;
         }
      }
      if((control & c::rxFlush) != 0) {
         buffers_.flushRx();
      }
      if((control & c::txDisable) != 0) {
         disableTx();
      } else if((control & c::txEnable) != 0) {
         if(!configured_) {
            ok = false;
         } else {
            txEnabled_ = true;
         }
      }
      if((control & c::txFlush) != 0) {
         uart_.stopTx();
         buffers_.flushTx();
      }
      return ok;
   }

   [[nodiscard]] uint8_t status() override {
      namespace s = cmsisdap::uartStatus;
      if(transport_ != cmsisdap::uartTransport::dap) {
         return 0;
      }
      uint8_t bits = 0;
      if(rxEnabled_) {
         bits |= s::rxEnabled;
      }
      if(txEnabled_) {
         bits |= s::txEnabled;
      }
      const Counters now = counters();
      if(now.lost != seen_.lost) {
         bits |= s::rxDataLost;
      }
      if(now.parity != seen_.parity) {
         bits |= s::parityError;
      }
      if(now.errors - now.parity != seen_.errors - seen_.parity) {
         bits |= s::framingError;
      }
      seen_ = now;
      return bits;
   }

   [[nodiscard]] uint32_t rxCount() const override {
      return isDap() && rxEnabled_ ? static_cast<uint32_t>(buffers_.rx.size()) : 0;
   }

   [[nodiscard]] uint32_t txCount() const override {
      return isDap() ? static_cast<uint32_t>(buffers_.tx.size()) : 0;
   }

   [[nodiscard]] std::size_t write(art::span<const uint8_t> data) override {
      if(!isDap() || !txEnabled_ || pending_ || data.empty()) {
         return 0;
      }
      const std::size_t count = buffers_.tx.try_push_bulk(data.data(), data.size());
      if(count != 0) {
         uart_.startTx();
      }
      return count;
   }

   [[nodiscard]] std::size_t read(art::span<uint8_t> data) override {
      if(!isDap() || !rxEnabled_ || data.empty()) {
         return 0;
      }
      return buffers_.rx.try_pop_bulk(data.data(), data.size());
   }

   void hostGone() override {
      static_cast<void>(setTransport(defaultTransport()));
   }

   private:
   struct Counters {
      uint32_t lost;
      uint32_t errors;
      uint32_t parity;
   };

   [[nodiscard]] uint8_t defaultTransport() const {
      return bridge_ != nullptr ? cmsisdap::uartTransport::comPort : cmsisdap::uartTransport::none;
   }

   [[nodiscard]] bool isDap() const {
      return transport_ == cmsisdap::uartTransport::dap;
   }

   [[nodiscard]] Counters counters() const {
      return { buffers_.rxDropped + buffers_.rxOverruns, buffers_.rxErrors, buffers_.rxParityErrors };
   }

   void disableTx() {
      if(txEnabled_) {
         uart_.stopTx();
         buffers_.flushTx();
      }
      txEnabled_ = false;
   }

   // The state of the DAP commands when they get the UART: not configured,
   // and no old error.
   void resetDap() {
      configured_ = false;
      rxEnabled_ = false;
      txEnabled_ = false;
      pending_.reset();
      seen_ = counters();
   }

   Uart &uart_;
   UartBuffers<Size> &buffers_;
   Bridge *bridge_ = nullptr;
   uint8_t transport_ = cmsisdap::uartTransport::none;
   bool configured_ = false;
   bool rxEnabled_ = false;
   bool txEnabled_ = false;
   // The format of the last DAP_UART_Configure, until the TX path is empty
   std::optional<FrameFormat> pending_;
   // The error counters at the last read of the status
   Counters seen_{};
};

} // namespace uart
