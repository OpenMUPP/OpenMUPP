#pragma once

// The USB-to-UART bridge (plan section 4): moves the data between the
// CDC-ACM serial port and the UART, and applies the line coding of the host
// to the UART. Hardware-independent: the UART is a template parameter, so
// the host tests run the bridge with the real CDC-ACM function and a fake
// UART.
//
// Data path:
//
//   host -> CDC RX queue -> poll() -> TX ring -> UART interrupt -> TX pin
//   RX pin -> UART interrupt -> RX ring -> poll() -> CDC TX queue -> host
//
// The UART interrupt fills and empties the rings, so the serial port does
// not lose data while the main loop does long work. A ring holds Size - 1
// bytes: about 89 ms of data at 115200 baud for Size 1024.
//
// Flow control:
// - Host to UART: poll() takes only as many bytes from the CDC port as the
//   TX ring can hold. The CDC function arms its OUT endpoint only when its
//   RX queue has room for a full packet, so with a full TX ring the
//   endpoint NAKs and the host waits. No byte is dropped.
// - UART to host: the UART has no flow control lines. When the RX ring is
//   full, the interrupt drops the new byte and counts it (rxDropped()).
//   The interrupt also counts the bytes that the UART lost in an overrun
//   (rxOverruns()), and the bytes with a framing, noise or parity error
//   (rxErrors()). A byte with an error still goes to the host.
//   While the host has not configured the device, poll() discards the
//   received bytes, so old data does not wait in the ring.
//
// Line coding: lineCodingRequested() maps the line coding with
// toFrameFormat() (uart/FrameFormat.h). It refuses a line coding that the
// UART cannot do: the request stalls and the UART keeps its format. The
// new format is applied in poll() after the TX ring and the UART are empty,
// so the bytes that the host sent before the change go out in the old
// format. Until then, poll() takes no new data from the host.
//
// DTR and RTS: the bridge stores the control line state of the host
// (controlLines()). It drives no pin (plan section 9).
//
// Owner: the bridge shares the UART and the rings with the DAP_UART_*
// commands (uart/DapUartChannel.h). Only the owner moves data. While the
// bridge is not the owner (setActive(false)), poll() discards the data of
// the host and sends nothing to the host. The bridge still takes a line
// coding, and setActive(true) applies the line coding to the UART again.
// The owner switch empties the rings (UartBuffers::flushTx(), flushRx()).
//
// Threading: all functions of UartBridge run in the main loop, the context
// of the CDC-ACM function. Only the interrupt-side functions of UartBuffers
// run in the UART interrupt.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <usb/CdcAcmFunction.h>
#include <utils/RingBuffer.h>

#include "uart/FrameFormat.h"

namespace uart {

// Size of each ring of the firmware: 2 x 1 KB of the 20 KB RAM (plan
// section 3).
inline constexpr std::size_t bridgeBufferSize = 1024;

// The two rings between the main loop and the UART interrupt. Each ring has
// one producer and one consumer (see utils/RingBuffer.h).
template <std::size_t Size>
struct UartBuffers {
   // Main loop (producer) to the interrupt (consumer)
   RingBuffer<uint8_t, Size> tx;
   // Interrupt (producer) to the main loop (consumer)
   RingBuffer<uint8_t, Size> rx;
   // Received bytes that the interrupt dropped because rx was full. Only
   // the interrupt writes it.
   volatile uint32_t rxDropped = 0;
   // Received bytes that the UART lost because the interrupt read the data
   // register too late (overrun). Only the interrupt writes it.
   volatile uint32_t rxOverruns = 0;
   // Received bytes with a framing, noise or parity error. Only the
   // interrupt writes it.
   volatile uint32_t rxErrors = 0;
   // rxErrors split in two: the bytes with a parity error, and the bytes
   // with a framing or noise error only. Each status bit of the DAP_UART_*
   // commands then compares one counter, so the main loop needs no
   // consistent read of two counters. Only the interrupt writes them.
   volatile uint32_t rxParityErrors = 0;
   volatile uint32_t rxFramingErrors = 0;

   // Interrupt side: stores a received byte.
   void received(uint8_t byte) {
      if(!rx.try_push(byte)) {
         rxDropped = rxDropped + 1;
      }
   }

   // Interrupt side: the UART lost a byte (overrun).
   void overrun() {
      rxOverruns = rxOverruns + 1;
   }

   // Interrupt side: the last received byte had a framing, noise or parity
   // error.
   void receiveError(bool parity = false) {
      rxErrors = rxErrors + 1;
      if(parity) {
         rxParityErrors = rxParityErrors + 1;
      } else {
         rxFramingErrors = rxFramingErrors + 1;
      }
   }

   // Interrupt side: the next byte to send. False when tx is empty.
   bool nextToSend(uint8_t &byte) {
      return tx.try_pop(byte);
   }

   // Main loop side: discards the bytes of tx. Only while the interrupt
   // does not send (TX interrupt off, Uart::stopTx()): the main loop is
   // then the only user of tx.
   void flushTx() {
      drain(tx);
   }

   // Main loop side: discards the bytes of rx. The main loop is the
   // consumer of rx, so the interrupt can receive at the same time.
   void flushRx() {
      drain(rx);
   }

   private:
   static void drain(RingBuffer<uint8_t, Size> &ring) {
      // The data can wrap around the end of the ring: two rounds.
      for(;;) {
         std::size_t length = 0;
         static_cast<void>(ring.read_ptr(length));
         if(length == 0) {
            return;
         }
         ring.commit_read(length);
      }
   }
};

// Uart is the UART driver (uart/UartPort.h) or a fake:
//   uint32_t clockFrequency() const;      clock of the UART in Hz
//   void setFormat(const FrameFormat &);  only called while isTxIdle()
//   bool isTxIdle() const;                TX interrupt off, last byte sent
//   void startTx();                       TX interrupt on: sends the tx ring
template <typename Uart, std::size_t Size>
class UartBridge final : public art::usb::CdcAcmEvents {
   public:
   // The format before the first SET_LINE_CODING: the default line coding
   // of the CDC-ACM function (115200 8N1).
   static constexpr FrameFormat defaultFormat{ 115200, DataBits::Eight, Parity::None, StopBits::One };

   UartBridge(art::usb::CdcAcmFunctionBase &port, Uart &uart, UartBuffers<Size> &buffers)
      : port_(port)
      , uart_(uart)
      , buffers_(buffers) {
   }

   UartBridge(const UartBridge &) = delete;
   UartBridge &operator=(const UartBridge &) = delete;

   // Sets the UART to the default format and receives the class requests
   // of the port. Call it once, before the USB device starts: the port
   // then still has the default line coding.
   void start() {
      uart_.setFormat(format_);
      port_.setEventHandler(this);
   }

   // Moves the data in both directions and applies a new line coding. Call
   // it in each main loop iteration, after UsbStack::poll().
   void poll() {
      if(!active_) {
         discardFromHost();
         return;
      }
      applyPendingFormat();
      if(!pending_ && !reapply_) {
         fromHost();
      }
      toHost();
   }

   // Makes the bridge the owner of the UART, or not (see the top of this
   // file). The caller empties the rings and stops the TX interrupt at the
   // switch. When the bridge becomes the owner, it sets the UART to its
   // format again once the TX path is empty.
   void setActive(bool active) {
      if(active && !active_) {
         reapply_ = true;
      }
      active_ = active;
   }

   [[nodiscard]] bool isActive() const {
      return active_;
   }

   // The format of the UART now.
   [[nodiscard]] const FrameFormat &format() const {
      return format_;
   }

   // True while a new format waits for the TX path to become empty.
   [[nodiscard]] bool hasPendingFormat() const {
      return pending_.has_value();
   }

   // The control line state of the host (CdcAcmFunctionBase::
   // kControlLineDtr, kControlLineRts).
   [[nodiscard]] uint16_t controlLines() const {
      return controlLines_;
   }

   [[nodiscard]] uint32_t rxDropped() const {
      return buffers_.rxDropped;
   }

   [[nodiscard]] uint32_t rxOverruns() const {
      return buffers_.rxOverruns;
   }

   [[nodiscard]] uint32_t rxErrors() const {
      return buffers_.rxErrors;
   }

   // CdcAcmEvents
   bool lineCodingRequested(const art::usb::LineCoding &requested) override {
      const auto format = toFrameFormat(requested, uart_.clockFrequency());
      if(!format) {
         return false;
      }
      // Hosts send SET_LINE_CODING each time a port opens. The same format
      // again does not touch the UART.
      if(*format == format_) {
         pending_.reset();
      } else {
         pending_ = *format;
      }
      return true;
   }

   void controlLineStateChanged(uint16_t state, uint16_t /*previousState*/) override {
      controlLines_ = state;
   }

   private:
   void applyPendingFormat() {
      if((!pending_ && !reapply_) || !buffers_.tx.empty() || !uart_.isTxIdle()) {
         return;
      }
      if(pending_) {
         format_ = *pending_;
         pending_.reset();
      }
      reapply_ = false;
      uart_.setFormat(format_);
   }

   // The CDC RX queue while another side owns the UART: the data is lost,
   // and the OUT endpoint does not hold the host.
   void discardFromHost() {
      std::array<uint8_t, 64> scrap{};
      while(port_.read(art::span<uint8_t>(scrap.data(), scrap.size())) != 0) {
      }
   }

   // CDC RX queue to the TX ring, as many bytes as the ring takes.
   void fromHost() {
      bool queued = false;
      // The free space of the ring can wrap around its end: two rounds.
      for(;;) {
         std::size_t room = 0;
         uint8_t *space = buffers_.tx.write_ptr(room);
         if(room == 0) {
            break;
         }
         const std::size_t count = port_.read(art::span<uint8_t>(space, room));
         if(count == 0) {
            break;
         }
         buffers_.tx.commit_write(count);
         queued = true;
      }
      if(queued) {
         uart_.startTx();
      }
   }

   // RX ring to the CDC TX queue, as many bytes as the queue takes.
   void toHost() {
      const bool configured = port_.isConfigured();
      for(;;) {
         std::size_t length = 0;
         const uint8_t *data = buffers_.rx.read_ptr(length);
         if(length == 0) {
            return;
         }
         const std::size_t sent = configured ? port_.write(art::span<const uint8_t>(data, length)) : length;
         buffers_.rx.commit_read(sent);
         if(sent < length) {
            return;
         }
      }
   }

   art::usb::CdcAcmFunctionBase &port_;
   Uart &uart_;
   UartBuffers<Size> &buffers_;
   FrameFormat format_ = defaultFormat;
   std::optional<FrameFormat> pending_;
   uint16_t controlLines_ = 0;
   bool active_ = true;
   // The UART must get format_ again: another owner had it.
   bool reapply_ = false;
};

} // namespace uart
