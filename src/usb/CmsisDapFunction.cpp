// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb/CmsisDapFunction.h"

#include <algorithm>

namespace usb {

template <typename Base>
bool DapFunction<Base>::poll() {
   // A packet that came before the suspend runs after the resume: the
   // target stays released during the suspend.
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

template <typename Base>
bool DapFunction<Base>::dataReceived(art::span<const uint8_t> packet) {
   // The OUT endpoint is held while no slot is free, so a packet that
   // arrives here always fits. poll() runs it.
   if(!packet.empty()) {
      ++commandCount_;
      processor_.submit(packet);
   }
   return processor_.canSubmit();
}

template <typename Base>
void DapFunction<Base>::transferSent() {
   processor_.popResponse();
   sendResponse();
   if(processor_.canSubmit()) {
      this->resumeOut();
   }
}

template <typename Base>
void DapFunction<Base>::dataPathOpened() {
   // No packet is in flight: dataPathClosed() emptied the processor.
   processor_.setPacketSize(this->packetSize());
}

template <typename Base>
void DapFunction<Base>::dataPathClosed() {
   suspended_ = false;
   processor_.hostGone();
}

template <typename Base>
void DapFunction<Base>::suspended() {
   // The USB function keeps its IN transfer and OUT hold over a suspend, so
   // the processor keeps its queue and responses. Only the target is
   // released.
   suspended_ = true;
   processor_.release();
}

template <typename Base>
void DapFunction<Base>::resumed() {
   suspended_ = false;
}

template <typename Base>
void DapFunction<Base>::sendResponse() {
   while(!this->isInTransferBusy() && processor_.hasResponse()) {
      const auto response = processor_.response();
      if(!response.empty() && sendPacket(response)) {
         return;
      }
      // Not configured, or nothing to send: drop it.
      processor_.popResponse();
   }
}

template <typename Base>
bool DapFunction<Base>::sendPacket(art::span<const uint8_t> response) {
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

template class DapFunction<art::usb::VendorBulkFunction>;
template class DapFunction<art::usb::HidFunction>;

} // namespace usb
