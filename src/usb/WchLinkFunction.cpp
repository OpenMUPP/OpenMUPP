#include "usb/WchLinkFunction.h"

#include <usb/Device.h>

namespace usb {

WchLinkFunction::WchLinkFunction(wchlink::Processor &processor)
   : Function(wch::wchInterface, 1)
   , processor_(processor) {
}

bool WchLinkFunction::poll() {
   if(!configured_ || suspended_) {
      return false;
   }
   if(processor_.runNext()) {
      // Send the response at once, before the next request.
      sendResponse();
      return true;
   }
   if(!data_.busy && processor_.hasData()) {
      sendData();
      return true;
   }
   return false;
}

void WchLinkFunction::configured() {
   reset();
   configured_ = true;
   armCommand();
}

void WchLinkFunction::deconfigured() {
   configured_ = false;
   suspended_ = false;
   reset();
   processor_.hostGone();
}

void WchLinkFunction::alternateSettingChanged(uint8_t interfaceNumber, uint8_t alternateSetting) {
   static_cast<void>(alternateSetting);
   // SET_INTERFACE closed and reopened the endpoints: the controller
   // dropped the OUT arm and the IN packets on their way.
   if(interfaceNumber == wch::wchInterface) {
      endpointHaltCleared(wch::commandOutEndpoint);
      endpointHaltCleared(wch::commandInEndpoint);
      endpointHaltCleared(wch::dataInEndpoint);
   }
}

void WchLinkFunction::suspended() {
   // The transfer state stays, so the processor keeps its request and
   // response. Only the target is released.
   suspended_ = true;
   processor_.release();
}

void WchLinkFunction::resumed() {
   suspended_ = false;
}

void WchLinkFunction::outPacketReceived(uint8_t endpointAddress, art::span<const uint8_t> packet) {
   if(endpointAddress != wch::commandOutEndpoint || !configured_) {
      return;
   }
   commandArmed_ = false;
   if(packet.empty() || !processor_.canSubmit()) {
      armCommand();
      return;
   }
   ++commandCount_;
   // poll() runs it. The endpoint stays unarmed until the response is sent.
   processor_.submit(packet);
}

void WchLinkFunction::inPacketSent(uint8_t endpointAddress) {
   if(endpointAddress == wch::commandInEndpoint && response_.busy) {
      response_ = {};
      processor_.popResponse();
      armCommand();
   } else if(endpointAddress == wch::dataInEndpoint && data_.busy) {
      data_ = {};
      processor_.dataSent();
   }
}

void WchLinkFunction::endpointHaltCleared(uint8_t endpointAddress) {
   if(endpointAddress == wch::commandOutEndpoint) {
      commandArmed_ = false;
      armCommand();
   } else if(endpointAddress == wch::commandInEndpoint && response_.busy) {
      static_cast<void>(start(wch::commandInEndpoint, response_));
   } else if(endpointAddress == wch::dataInEndpoint && data_.busy) {
      static_cast<void>(start(wch::dataInEndpoint, data_));
   }
}

void WchLinkFunction::armCommand() {
   art::usb::Device *usb = device();
   if(!configured_ || commandArmed_ || !processor_.canSubmit() || usb == nullptr || usb->isEndpointHalted(wch::commandOutEndpoint)) {
      return;
   }
   usb->armOutTransfer(wch::commandOutEndpoint);
   commandArmed_ = true;
}

void WchLinkFunction::sendResponse() {
   if(response_.busy || !processor_.hasResponse()) {
      return;
   }
   response_ = { processor_.response(), true };
   if(!start(wch::commandInEndpoint, response_)) {
      // Not configured: drop it.
      response_ = {};
      processor_.popResponse();
      armCommand();
   }
}

void WchLinkFunction::sendData() {
   data_ = { processor_.dataPacket(), true };
   if(data_.data.empty() || !start(wch::dataInEndpoint, data_)) {
      data_ = {};
      processor_.dataSent();
   }
}

bool WchLinkFunction::start(uint8_t endpoint, InPacket &in) {
   art::usb::Device *usb = device();
   if(usb == nullptr) {
      return false;
   }
   // On a halted endpoint, the packet waits for the clear of the halt.
   return usb->isEndpointHalted(endpoint) || usb->startInTransfer(endpoint, in.data);
}

void WchLinkFunction::reset() {
   commandArmed_ = false;
   response_ = {};
   data_ = {};
}

} // namespace usb
