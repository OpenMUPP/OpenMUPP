#pragma once

// Log sink of a debug build (VSL_DEBUG_CDC): the libArT logger writes the
// log lines into a fixed ring buffer, and drainTo() moves them to the
// serial port in the main loop. Hardware-independent, so the host tests
// check it with a fake port.
//
// write() never waits. When the ring buffer is full, write() drops the new
// bytes (drop newest): the old lines stay complete and the start-up log is
// kept until a terminal opens the port. When the ring buffer has room
// again, the sink first writes a marker line with the number of dropped
// bytes.
//
// Threading: write() and drainTo() must come from the main loop, the same
// context as the CDC-ACM function. Do not log from an interrupt handler.

#include <array>
#include <cstddef>
#include <cstdint>
#include <streams/Writable.h>
#include <utils/RingBuffer.h>

namespace debug {

// Port is the CDC-ACM function (art::usb::CdcAcmFunctionBase) or a fake:
//   bool isTerminalOpen() const;
//   std::size_t writeAvailable() const;
//   std::size_t write(art::span<const uint8_t> data);
template <std::size_t Size>
class LogSink final : public Writable {
   public:
   // The marker line: "\r\n[log: <count> bytes dropped]\r\n"
   static constexpr char markerPrefix[] = "\r\n[log: ";
   static constexpr char markerSuffix[] = " bytes dropped]\r\n";
   // A 32-bit count has at most 10 digits.
   static constexpr std::size_t maxMarkerSize = sizeof(markerPrefix) - 1 + 10 + sizeof(markerSuffix) - 1;
   static_assert(Size > maxMarkerSize, "the ring buffer must hold the dropped-bytes marker");

   // Always reports all bytes as written: a full ring buffer drops them
   // (see droppedBytes()), it is not an error for the logger.
   art::expected<size_t, Error> write(art::span<const uint8_t> data) override {
      if(droppedBytes_ != 0) {
         if(!writeMarker()) {
            droppedBytes_ += static_cast<uint32_t>(data.size());
            return data.size();
         }
      }
      const std::size_t taken = buffer_.try_push_bulk(data.data(), data.size());
      droppedBytes_ += static_cast<uint32_t>(data.size() - taken);
      return data.size();
   }

   // The logger flushes after each line. The bytes leave in drainTo().
   art::expected<void, Error> flush() override {
      return {};
   }

   // Moves buffered bytes to the port, as many as the port takes now. Moves
   // nothing while no terminal has the port open (DTR off), so the bytes
   // wait in the ring buffer and not in the port queue. Call it in each main
   // loop iteration.
   template <typename Port>
   void drainTo(Port &port) {
      if(!port.isTerminalOpen()) {
         return;
      }
      for(;;) {
         const std::size_t room = port.writeAvailable();
         std::size_t length = 0;
         const uint8_t *data = buffer_.read_ptr(length);
         length = length < room ? length : room;
         if(length == 0) {
            return;
         }
         const std::size_t sent = port.write(art::span<const uint8_t>(data, length));
         buffer_.commit_read(sent);
         if(sent < length) {
            return;
         }
      }
   }

   // Bytes dropped since the last marker line.
   [[nodiscard]] uint32_t droppedBytes() const {
      return droppedBytes_;
   }

   // Bytes in the ring buffer.
   [[nodiscard]] std::size_t pending() const {
      return buffer_.size();
   }

   [[nodiscard]] static constexpr std::size_t capacity() {
      return decltype(buffer_)::capacity();
   }

   private:
   // Writes the dropped-bytes marker if it fits completely. Clears the
   // count on success.
   bool writeMarker() {
      std::array<uint8_t, maxMarkerSize> marker{};
      std::size_t length = append(marker, 0, markerPrefix);
      length = appendDecimal(marker, length, droppedBytes_);
      length = append(marker, length, markerSuffix);
      if(buffer_.space_available() < length) {
         return false;
      }
      static_cast<void>(buffer_.try_push_bulk(marker.data(), length));
      droppedBytes_ = 0;
      return true;
   }

   static std::size_t append(std::array<uint8_t, maxMarkerSize> &out, std::size_t at, const char *text) {
      while(*text != '\0' && at < out.size()) {
         out[at++] = static_cast<uint8_t>(*text++);
      }
      return at;
   }

   static std::size_t appendDecimal(std::array<uint8_t, maxMarkerSize> &out, std::size_t at, uint32_t value) {
      std::array<uint8_t, 10> digits{};
      std::size_t count = 0;
      do {
         digits[count++] = static_cast<uint8_t>('0' + value % 10);
         value /= 10;
      } while(value != 0);
      while(count != 0 && at < out.size()) {
         out[at++] = digits[--count];
      }
      return at;
   }

   RingBuffer<uint8_t, Size> buffer_;
   uint32_t droppedBytes_ = 0;
};

} // namespace debug
