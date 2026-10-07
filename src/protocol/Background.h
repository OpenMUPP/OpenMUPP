#pragma once

// Work that must continue while a long USB_TO_XXX command runs: the serial
// port of the UART bridge. A command runs in the main loop, inside
// UsbStack::poll(), so the main loop stops until the command ends. A
// JTAG_RAW shift at 10 kHz or an SWD batch with many WAIT retries can take
// seconds, but the RX ring of the UART bridge holds only about 89 ms at
// 115200 baud (uart/UartBridge.h).
//
// The bit-bang engines (jtag/JtagBitBang.h, swd/SwdBitBang.h) call
// Background::service() at a safe point of the bit stream, at most once
// per backgroundIntervalMs:
//
// - JTAG: between bytes of a shift, with TCK low.
// - SWD: before each transfer and each retry, and between bytes of a
//   sequence, with SWCLK high.
//
// The call makes the clock phase at that point longer. The clock does not
// get faster: each half period waits from its edge. service() must not
// start a USB_TO_XXX command (no re-entry into the dispatcher), and must
// return within some microseconds.

#include <cstdint>

namespace protocol {

class Background {
   public:
   virtual void service() = 0;

   protected:
   ~Background() = default;
};

// The time between two Background::service() calls of an engine. In each
// call, the serial port sends one IN packet: 64 bytes per ms is more than
// the 11.5 bytes per ms of 115200 baud.
inline constexpr uint32_t backgroundIntervalMs = 1;

// Calls a Background from a bit-bang loop, at most once per
// backgroundIntervalMs. Pins has now() and coreHz() (see the engines). The
// interval runs on over shifts and transfers: a command with many short
// sub-commands is also serviced.
class BackgroundTimer {
   public:
   explicit BackgroundTimer(Background *background)
      : background_(background) {
   }

   // Call it at the start of each shift or transfer.
   template <typename Pins>
   void begin(Pins &pins) {
      intervalCycles_ = pins.coreHz() / 1000u * backgroundIntervalMs;
   }

   template <typename Pins>
   [[gnu::always_inline]] void poll(Pins &pins) {
      if(background_ == nullptr) {
         return;
      }
      // Unsigned difference: correct across the wrap of the counter.
      if(pins.now() - last_ >= intervalCycles_) {
         background_->service();
         last_ = pins.now();
      }
   }

   private:
   Background *background_;
   uint32_t intervalCycles_{ 0 };
   uint32_t last_{ 0 };
};

} // namespace protocol
