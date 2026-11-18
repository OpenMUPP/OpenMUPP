// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The CMSIS-DAP interfaces: usb::CmsisDapFunction is the CMSIS-DAP v2
// interface (interface 0, bulk OUT 0x03, bulk IN 0x82), and
// usb::CmsisDapHidFunction the CMSIS-DAP v1 interface (HID, interrupt OUT
// 0x05, interrupt IN 0x85, MUPP_CMSIS_DAP_HID). Both are DapFunction on
// another libArT base, with the same data path.
// Hardware-independent: the host unit tests run them on a fake controller.
//
// Each OUT packet is one command and goes to the processor
// (cmsisdap::Processor::submit()). poll() runs one command in the main
// loop (cmsisdap::Processor::runNext()), outside the delivery of the OUT
// packet. Thus the OUT endpoint is armed while a command runs. Each
// response of the processor is sent as one IN packet, in order. A response of the full packet size has no ZLP:
// the host reads one packet per response. An empty OUT packet (a ZLP) is
// ignored.
//
// On HID, each packet is one report of 64 bytes at both speeds. The host
// pads a command with zeros, and the function pads each response with
// zeros to the report size.
//
// The processor holds up to Processor::packetCount packets that have no
// response sent yet. When no slot is free, the OUT endpoint NAKs until a
// response is sent. DAP_Info reports the same packet count to the host.
//
// The packet size is the wMaxPacketSize of the endpoints at the bus
// speed: on bulk 64 bytes at full speed and 512 bytes at high speed, on
// HID 64 bytes. The function gives it to the processor at
// SET_CONFIGURATION, and DAP_Info reports it.
//
// When the host closes the interface (bus reset or deconfigure), the
// function calls Processor::hostGone(): it disconnects the target, releases
// the pins, and drops the queued packets and the unsent responses. A suspend
// only releases the target (Processor::release()): the USB transfer state
// stays, so the processor keeps its packets and responses. poll() runs no
// command until the resume.
//
// A DAP_QueueCommands packet in the last free slot runs the queue in the
// processor, so queued packets alone never hold the OUT endpoint.
//
// withSwo (bulk only): the interface has the SWO trace endpoint (bulk IN
// 0x86, usb/CmsisDapDescriptors.h). poll() sends the trace data of
// DAP_SWO_Transport 2 (cmsisdap::Processor::readSwoStream()) on it, one
// packet of up to wMaxPacketSize bytes at a time, and the acknowledge of a
// packet sends the next one at once. A packet that the controller drops
// (halt clear, SET_INTERFACE) goes out again. A bus reset or deconfigure
// drops the packet on its way.

#include <array>
#include <cstdint>
#include <type_traits>
#include <usb/HidFunction.h>
#include <usb/VendorBulkFunction.h>

#include "protocol/cmsisdap/DapProcessor.h"
#include "usb/CmsisDapDescriptors.h"

namespace usb {

template <typename Base, bool withSwo = false>
class DapFunction final : public Base {
   public:
   static constexpr bool isHid = std::is_base_of_v<art::usb::HidFunction, Base>;
   static_assert(!withSwo || !isHid, "The SWO trace endpoint is on the CMSIS-DAP v2 interface");

   static_assert(dap::packetSize == cmsisdap::Processor::fullSpeedPacketSize, "The USB packet carries one DAP packet");
   static_assert(dap::highSpeedPacketSize >= cmsisdap::Processor::maxPacketSize, "A DAP packet fits a USB packet");
   static_assert(dap::hidReportSize == cmsisdap::Processor::fullSpeedPacketSize, "A HID report carries one DAP packet");

   // The processor must outlive the function.
   explicit DapFunction(cmsisdap::Processor &processor)
      requires(!isHid)
      : Base(dap::dapInterface,
             art::usb::VendorBulkEndpoints{ .out = dap::outEndpoint, .in = dap::inEndpoint, .maxPacketSize = dap::packetSize })
      , processor_(processor) {
   }

   explicit DapFunction(cmsisdap::Processor &processor)
      requires(isHid)
      : Base(dap::hidInterface,
             art::usb::VendorBulkEndpoints{ .out = dap::hidOutEndpoint, .in = dap::hidInEndpoint, .maxPacketSize = dap::hidReportSize },
             desc::asSpan(dap::hidReportDescriptor))
      , processor_(processor) {
   }

   // Runs the next received command and sends its response. Returns false
   // when no command ran. Call it in the main loop after the USB events,
   // not during a command. Deliver the USB events again before the next
   // call: the IN endpoint is free for the next response only after the
   // InComplete event.
   bool poll();

   // Number of command packets received. It wraps at 2^32.
   [[nodiscard]] uint32_t commandCount() const {
      return commandCount_;
   }

   protected:
   bool dataReceived(art::span<const uint8_t> packet) override;
   void transferSent() override;
   void dataPathOpened() override;
   void dataPathClosed() override;
   void suspended() override;
   void resumed() override;
   void otherInPacketSent(uint8_t endpointAddress) override;
   void otherInPacketsDropped(uint8_t endpointAddress) override;

   private:
   struct NoReport {};
   // The packet on the SWO trace endpoint
   struct SwoPacket {
      std::array<uint8_t, dap::highSpeedPacketSize> data{};
      // The bytes that the host did not acknowledge yet; 0: no packet
      uint16_t length{ 0 };
      // The controller has the packet
      bool inFlight{ false };
   };

   // Sends the next packet of trace data, or the dropped packet again, if
   // the SWO trace endpoint is free.
   void sendSwo();

   // Starts the IN transfer of the oldest response, if the IN endpoint is
   // free. A response that cannot be sent (not configured) is dropped.
   void sendResponse();
   // Starts the IN transfer of one response. False when it did not start.
   bool sendPacket(art::span<const uint8_t> response);

   cmsisdap::Processor &processor_;
   // Between suspended() and resumed(): poll() runs no command.
   bool suspended_{ false };
   uint32_t commandCount_{ 0 };
   // HID: the response on its way, padded to the report size
   [[no_unique_address]] std::conditional_t<isHid, std::array<uint8_t, dap::hidReportSize>, NoReport> report_{};
   [[no_unique_address]] std::conditional_t<withSwo, SwoPacket, NoReport> swo_{};
};

using CmsisDapFunction = DapFunction<art::usb::VendorBulkFunction, dap::hasSwo>;
using CmsisDapHidFunction = DapFunction<art::usb::HidFunction>;

extern template class DapFunction<art::usb::VendorBulkFunction, false>;
extern template class DapFunction<art::usb::VendorBulkFunction, true>;
extern template class DapFunction<art::usb::HidFunction>;

} // namespace usb
