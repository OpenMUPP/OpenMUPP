#include "usb/CmsisDapFunction.h"

namespace usb {

CmsisDapFunction::CmsisDapFunction(cmsisdap::Processor &processor)
   : VendorBulkFunction(dap::dapInterface,
                        art::usb::VendorBulkEndpoints{ .out = dap::outEndpoint, .in = dap::inEndpoint, .maxPacketSize = dap::packetSize })
   , processor_(processor) {
}

void CmsisDapFunction::poll() {
   // A packet that came before the suspend runs after the resume: the
   // target stays released during the suspend.
   if(suspended_) {
      return;
   }
   // Send each response at once, before the next packet runs.
   while(processor_.runNext()) {
      sendResponse();
   }
   sendResponse();
   if(processor_.canSubmit()) {
      resumeOut();
   }
}

bool CmsisDapFunction::dataReceived(art::span<const uint8_t> packet) {
   // The OUT endpoint is held while no slot is free, so a packet that
   // arrives here always fits. poll() runs it.
   if(!packet.empty()) {
      ++commandCount_;
      processor_.submit(packet);
   }
   return processor_.canSubmit();
}

void CmsisDapFunction::transferSent() {
   processor_.popResponse();
   sendResponse();
   if(processor_.canSubmit()) {
      resumeOut();
   }
}

void CmsisDapFunction::dataPathClosed() {
   suspended_ = false;
   processor_.hostGone();
}

void CmsisDapFunction::suspended() {
   // The USB function keeps its IN transfer and OUT hold over a suspend, so
   // the processor keeps its queue and responses. Only the target is
   // released.
   suspended_ = true;
   processor_.release();
}

void CmsisDapFunction::resumed() {
   suspended_ = false;
}

void CmsisDapFunction::sendResponse() {
   while(!isInTransferBusy() && processor_.hasResponse()) {
      const auto response = processor_.response();
      if(!response.empty() && sendInTransfer(response, ZeroLengthPacket::Never)) {
         return;
      }
      // Not configured, or nothing to send: drop it.
      processor_.popResponse();
   }
}

} // namespace usb
