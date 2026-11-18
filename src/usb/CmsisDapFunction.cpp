// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb/CmsisDapFunction.h"

#include <algorithm>
#include <usb/Device.h>

namespace usb {

template <typename Base, bool withSwo>
bool DapFunction<Base, withSwo>::poll() {
   // A packet that came before the suspend runs after the resume: the
   // target stays released during the suspend.
   if constexpr(withSwo) {
      sendSwo();
   }
   if(suspended_ || !processor_.runNext()) {
      return false;
   }
   // Send the response at once, before the next packet runs.
   sendResponse();
   if(processor_.canSubmit()) {
      this->resumeOut();
   }
   return true;
}

template <typename Base, bool withSwo>
bool DapFunction<Base, withSwo>::dataReceived(art::span<const uint8_t> packet) {
   // The OUT endpoint is held while no slot is free, so a packet that
   // arrives here always fits. poll() runs it.
   if(!packet.empty()) {
      ++commandCount_;
      processor_.submit(packet);
   }
   return processor_.canSubmit();
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::transferSent() {
   processor_.popResponse();
   sendResponse();
   if(processor_.canSubmit()) {
      this->resumeOut();
   }
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::dataPathOpened() {
   // No packet is in flight: dataPathClosed() emptied the processor.
   processor_.setPacketSize(this->packetSize());
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::dataPathClosed() {
   suspended_ = false;
   if constexpr(withSwo) {
      // The controller dropped the packet on its way.
      swo_.length = 0;
      swo_.inFlight = false;
   }
   processor_.hostGone();
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::suspended() {
   // The USB function keeps its IN transfer and OUT hold over a suspend, so
   // the processor keeps its queue and responses. Only the target is
   // released.
   suspended_ = true;
   processor_.release();
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::resumed() {
   suspended_ = false;
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::otherInPacketSent(uint8_t endpointAddress) {
   if constexpr(withSwo) {
      if(endpointAddress == dap::swoEndpoint) {
         swo_.length = 0;
         swo_.inFlight = false;
         sendSwo();
      }
   } else {
      static_cast<void>(endpointAddress);
   }
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::otherInPacketsDropped(uint8_t endpointAddress) {
   if constexpr(withSwo) {
      // 0: SET_INTERFACE reopened all endpoints
      if(endpointAddress == dap::swoEndpoint || endpointAddress == 0) {
         swo_.inFlight = false;
         sendSwo();
      }
   } else {
      static_cast<void>(endpointAddress);
   }
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::sendSwo() {
   if constexpr(withSwo) {
      art::usb::Device *const device = this->device();
      if(suspended_ || swo_.inFlight || !this->isConfigured() || device == nullptr || device->isEndpointHalted(dap::swoEndpoint)) {
         return;
      }
      const std::size_t size = std::min<std::size_t>(device->endpointMaxPacketSize(dap::swoEndpoint), swo_.data.size());
      if(swo_.length == 0) {
         swo_.length = static_cast<uint16_t>(processor_.readSwoStream({ swo_.data.data(), size }));
      }
      if(swo_.length != 0) {
         swo_.inFlight = device->startInTransfer(dap::swoEndpoint, { swo_.data.data(), swo_.length });
      }
   }
}

template <typename Base, bool withSwo>
void DapFunction<Base, withSwo>::sendResponse() {
   while(!this->isInTransferBusy() && processor_.hasResponse()) {
      const auto response = processor_.response();
      if(!response.empty() && sendPacket(response)) {
         return;
      }
      // Not configured, or nothing to send: drop it.
      processor_.popResponse();
   }
}

template <typename Base, bool withSwo>
bool DapFunction<Base, withSwo>::sendPacket(art::span<const uint8_t> response) {
   if constexpr(isHid) {
      // The processor packet size is the report size, so the response fits.
      const std::size_t count = std::min(response.size(), report_.size());
      std::copy_n(response.begin(), count, report_.begin());
      std::fill(report_.begin() + static_cast<std::ptrdiff_t>(count), report_.end(), uint8_t{ 0 });
      return this->sendReport(art::span<const uint8_t>(report_.data(), report_.size()));
   } else {
      return this->sendInTransfer(response, Base::ZeroLengthPacket::Never);
   }
}

template class DapFunction<art::usb::VendorBulkFunction, false>;
template class DapFunction<art::usb::VendorBulkFunction, true>;
template class DapFunction<art::usb::HidFunction>;

} // namespace usb
