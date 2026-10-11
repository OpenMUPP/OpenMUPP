#pragma once

// The CMSIS-DAP v2 interface (interface 0, bulk OUT 0x03, bulk IN 0x82).
// Hardware-independent: the host unit tests run it on a fake controller.
//
// Each OUT packet is one command and goes to the processor
// (cmsisdap::Processor::submit()). poll() runs the commands in the main
// loop (cmsisdap::Processor::run()), outside the delivery of the OUT
// packet. Thus the OUT endpoint is armed while a command runs. Each
// response of the processor is sent as one IN packet, in order. A response of the full packet size has no ZLP:
// the host reads one packet per response. An empty OUT packet (a ZLP) is
// ignored.
//
// The processor holds up to Processor::packetCount packets that have no
// response sent yet. When no slot is free, the OUT endpoint NAKs until a
// response is sent. DAP_Info reports the same packet count to the host.
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

#include <cstdint>
#include <usb/VendorBulkFunction.h>

#include "protocol/cmsisdap/DapProcessor.h"
#include "usb/CmsisDapDescriptors.h"

namespace usb {

class CmsisDapFunction final : public art::usb::VendorBulkFunction {
   public:
   static_assert(dap::packetSize == cmsisdap::Processor::packetSize, "The USB packet carries one DAP packet");

   // The processor must outlive the function.
   explicit CmsisDapFunction(cmsisdap::Processor &processor);

   // Runs the received commands and sends their responses. Call it in the
   // main loop after the USB events, not during a command.
   void poll();

   // Number of command packets received. It wraps at 2^32.
   [[nodiscard]] uint32_t commandCount() const {
      return commandCount_;
   }

   protected:
   bool dataReceived(art::span<const uint8_t> packet) override;
   void transferSent() override;
   void dataPathClosed() override;
   void suspended() override;
   void resumed() override;

   private:
   // Starts the IN transfer of the oldest response, if the IN endpoint is
   // free. A response that cannot be sent (not configured) is dropped.
   void sendResponse();

   cmsisdap::Processor &processor_;
   // Between suspended() and resumed(): poll() runs no command.
   bool suspended_{ false };
   uint32_t commandCount_{ 0 };
};

} // namespace usb
