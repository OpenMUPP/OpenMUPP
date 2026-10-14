#pragma once

// The WCH-Link interface (interface 0, usb/WchLinkDescriptors.h).
// Hardware-independent: the host unit tests run it on a fake controller.
//
// The interface has two bulk pairs, so the function is an art::usb::Function
// of its own: art::usb::VendorBulkFunction has one pair only.
//
// - Command OUT 0x01: each packet is one request and goes to the processor
//   (wchlink::Processor::submit()). The endpoint is armed again only after
//   the response is sent: one request at a time. An empty packet (a ZLP) is
//   ignored.
// - Command IN 0x81: each response is one IN packet, without a ZLP.
// - Data IN 0x82: the data of a memory read, in packets of 64 bytes. The
//   last packet can be shorter. No ZLP: the host reads the length that it
//   asked for.
// - Data OUT 0x02: the flash data (W5). The function does not arm it, so a
//   host that sends data gets a NAK until its timeout.
//
// poll() runs the request in the main loop (wchlink::Processor::runNext()),
// outside the delivery of the OUT packet, and reads the next data packet
// from the target when the data IN endpoint is free.
//
// When the host closes the interface (bus reset or deconfigure), the
// function calls Processor::hostGone(). A suspend only releases the target
// (Processor::release()), and poll() does nothing until the resume.
//
// A halt that the host clears, or SET_INTERFACE, drops the transfers of the
// endpoints. The function then arms the command OUT endpoint again, and
// sends the dropped IN packet again.

#include <cstdint>
#include <usb/Function.h>
#include <utils/compat/span.h>

#include "protocol/wchlink/WchLinkProcessor.h"
#include "usb/WchLinkDescriptors.h"

namespace usb {

class WchLinkFunction final : public art::usb::Function {
   public:
   static_assert(wch::packetSize == wchlink::Processor::packetSize, "The USB packet carries one WCH-Link packet");

   // The processor must outlive the function.
   explicit WchLinkFunction(wchlink::Processor &processor);

   // Runs the received request and sends its response, or sends the next
   // data packet. Returns false when it did nothing. Call it in the main
   // loop after the USB events, not during a command. Deliver the USB events
   // again before the next call.
   bool poll();

   // Number of request packets received. It wraps at 2^32.
   [[nodiscard]] uint32_t commandCount() const {
      return commandCount_;
   }

   void configured() override;
   void deconfigured() override;
   void alternateSettingChanged(uint8_t interfaceNumber, uint8_t alternateSetting) override;
   void suspended() override;
   void resumed() override;
   void outPacketReceived(uint8_t endpointAddress, art::span<const uint8_t> packet) override;
   void inPacketSent(uint8_t endpointAddress) override;
   void endpointHaltCleared(uint8_t endpointAddress) override;

   private:
   // An IN endpoint with one packet on its way
   struct InPacket {
      art::span<const uint8_t> data{};
      bool busy{ false };
   };

   void armCommand();
   void sendResponse();
   void sendData();
   // Starts the packet on the endpoint. Returns false when the controller
   // refused it.
   bool start(uint8_t endpoint, InPacket &in);
   void reset();

   wchlink::Processor &processor_;
   bool configured_{ false };
   bool suspended_{ false };
   bool commandArmed_{ false };
   InPacket response_;
   InPacket data_;
   uint32_t commandCount_{ 0 };
};

} // namespace usb
