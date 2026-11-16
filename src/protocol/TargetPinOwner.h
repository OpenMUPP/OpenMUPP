// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The owner of the target pins (SWD, JTAG and the AVR pins share them).
// Two USB interfaces of one image can drive the pins: the CMSIS-DAP v2
// bulk interface and the HID interface (CMSIS-DAP v1 and the AVR
// commands). The first interface that drives the pins owns them. The other
// gets "busy" until the owner releases them: DAP_Disconnect, or the host
// of the owner goes away. A host tool that stops without DAP_Disconnect
// (killed, crashed) keeps the pins until a bus reset or a replug: closing
// a libusb handle does not reach the device.
//
// The owner is an interface, not a protocol: avrdude sends DAP_Connect and
// the AVR commands on the same HID interface in one session.

#include <cstdint>

namespace protocol {

// The users of the target pins
enum class PinClient : uint8_t {
   None,
   CmsisDapBulk,
   CmsisDapHid,
   // The board keeps the pins for all time: its debug pins must not become
   // outputs (board::Definition::debugOutputs).
   Board,
};

class TargetPinOwner {
   public:
   // Takes the pins for the client. True when the client owns them after
   // the call: they were free or the client owned them already.
   [[nodiscard]] bool acquire(PinClient client) {
      if(owner_ == PinClient::None) {
         owner_ = client;
      }
      return owner_ == client;
   }

   // Gives the pins back. No effect when the client does not own them.
   void release(PinClient client) {
      if(owner_ == client) {
         owner_ = PinClient::None;
      }
   }

   // True when the client owns the pins, or nobody owns them
   [[nodiscard]] bool isFreeFor(PinClient client) const {
      return owner_ == PinClient::None || owner_ == client;
   }

   [[nodiscard]] PinClient owner() const {
      return owner_;
   }

   private:
   PinClient owner_{ PinClient::None };
};

} // namespace protocol
