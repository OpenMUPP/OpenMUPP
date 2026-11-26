// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The direction logic of a single-wire UART behind a TX level shifter with
// a direction pin (OpenMUPP v1: U6 and UART_TX_DIR, issues #148 and #152).
// Hardware-independent: the pins are a template parameter, so the host
// tests record the pin changes.
//
// Single-wire mode uses the half-duplex mode of the UART (HDSEL): the UART
// sends and receives on the TX pin. The target line needs a pull-up (on the
// board from revision 2). While the UART does not send, the TX shifter does
// not drive the target: with the direction pin low, the shifter drives the
// TX pin of the MCU, and the TX pin is open drain, so it does not drive
// against the shifter. While the UART sends, the receiver is off, so the
// probe does not receive its own data (no echo).
//
// Order of the changes:
// - Start of a transmission: receiver off, shifter drives the target, TX
//   pin push-pull. The open-drain TX pin idles high (external pull-up), so
//   the shifter starts with a high line.
// - End (the UART sent the last stop bit, TC): TX pin open drain, shifter
//   releases the target, receiver on.
// The TX pin is push-pull only while the shifter drives the target. Else
// the shifter drives the TX pin and the two outputs collide. Push-pull
// while the UART sends, because the external pull-up of the TX pin (100
// kOhm on OpenMUPP v1) gives a rising edge that is too slow for 1 Mbaud.
//
// A mode change while the UART sends takes effect at the end of the
// transmission. Thus the line does not turn around in a frame.
//
// Pins:
//   void driveTarget(bool on);     direction pin: on drives the target
//   void pushPullTx(bool on);      TX pin: on push-pull, off open drain
//   void halfDuplex(bool on);      UART half-duplex mode (HDSEL)
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
         setHalfDuplex(true);
         release();
      } else {
         // The receiver is on: the line is not sending.
         pins_.driveTarget(true);
         pins_.pushPullTx(true);
         setHalfDuplex(false);
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
      pins_.pushPullTx(true);
   }

   // The UART sent the last stop bit, and no block follows.
   void txComplete() {
      if(!sending_) {
         return;
      }
      sending_ = false;
      if(singleWire_) {
         // A change to single-wire mode while the UART sent in normal mode sets HDSEL now.
         setHalfDuplex(true);
         release();
      } else {
         // A change to normal mode while the line sent: the pins already drive.
         setHalfDuplex(false);
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
   void setHalfDuplex(bool on) {
      if(on != halfDuplex_) {
         halfDuplex_ = on;
         pins_.halfDuplex(on);
      }
   }

   void release() {
      pins_.pushPullTx(false);
      pins_.driveTarget(false);
      pins_.enableReceiver(true);
   }

   Pins &pins_;
   bool singleWire_ = false;
   bool halfDuplex_ = false;
   // Written in the UART interrupts and, with the interrupts locked, in the main loop
   volatile bool sending_ = false;
};

} // namespace uart
