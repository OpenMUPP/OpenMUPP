// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The direction logic of a single-wire UART behind a TX level shifter with
// a direction pin (OpenMUPP v1: U6 and UART_TX_DIR, issue #148).
// Hardware-independent: the pins are a template parameter, so the host
// tests record the pin changes.
//
// Single-wire mode needs a link from the TX pin to the RX pin of the target
// connector. RX reads the joined line through its fixed shifter. While the
// UART does not send, the TX shifter does not drive the target, and the TX
// pin of the MCU is an input: with the direction pin low, the shifter
// drives that pin. While the UART sends, the receiver is off, so the probe
// does not receive its own data (no echo).
//
// Order of the changes:
// - Start of a transmission: receiver off, shifter drives the target, TX
//   pin to the UART. The TX pin then idles high into a driving shifter.
// - End (the UART sent the last stop bit, TC): TX pin input, shifter
//   releases the target, receiver on.
// The TX pin is the UART output only while the shifter drives the target.
// Else the shifter drives the TX pin and the two outputs collide.
//
// A mode change while the UART sends takes effect at the end of the
// transmission. Thus the line does not turn around in a frame.
//
// Pins:
//   void driveTarget(bool on);     direction pin: on drives the target
//   void connectTx(bool on);       TX pin: on is the UART output, off input
//   void enableReceiver(bool on);  receiver of the UART

namespace uart {

template <typename Pins>
class SingleWireLine {
   public:
   explicit SingleWireLine(Pins &pins)
      : pins_(pins) {
   }

   // Sets the mode. busy: the UART sends now, so the change waits for
   // txComplete().
   void setSingleWire(bool on, bool busy) {
      if(on == singleWire_) {
         return;
      }
      singleWire_ = on;
      if(busy) {
         sending_ = true;
      } else if(on) {
         release();
      } else {
         // The receiver is on: the line is not sending.
         pins_.driveTarget(true);
         pins_.connectTx(true);
      }
   }

   // The UART starts to send a block.
   void txStart() {
      if(!singleWire_ || sending_) {
         return;
      }
      sending_ = true;
      pins_.enableReceiver(false);
      pins_.driveTarget(true);
      pins_.connectTx(true);
   }

   // The UART sent the last stop bit, and no block follows.
   void txComplete() {
      if(!sending_) {
         return;
      }
      sending_ = false;
      if(singleWire_) {
         release();
      } else {
         // A change to normal mode while the line sent: the pins already drive.
         pins_.enableReceiver(true);
      }
   }

   [[nodiscard]] bool singleWire() const {
      return singleWire_;
   }

   // True until txComplete(): the UART must report the end of the
   // transmission.
   [[nodiscard]] bool sending() const {
      return sending_;
   }

   private:
   void release() {
      pins_.connectTx(false);
      pins_.driveTarget(false);
      pins_.enableReceiver(true);
   }

   Pins &pins_;
   bool singleWire_ = false;
   // Written in the UART interrupts and, with the interrupts locked, in the main loop
   volatile bool sending_ = false;
};

} // namespace uart
