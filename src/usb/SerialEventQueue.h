#pragma once

// The event queue between the USB interrupt and the controller
// (Stm32UsbDeviceController::poll()), with a serial-only mode for
// UsbStack::pollSerial(). Hardware-independent: the host unit tests check
// it.
//
// pollSerial() runs during a long USB_TO_XXX command (protocol/
// Background.h), inside the delivery of the Versaloon OUT packet that
// started the command. It must not start a command, and must not close the
// Versaloon interface under the command. Thus, in the serial-only mode,
// try_pop() gives the event at the front of the queue only when it is an
// Out or InComplete event of an endpoint of the serial port. It stops at
// the first other event (SETUP, bus reset, suspend, resume, Versaloon
// endpoint). That event and all events after it wait for the next
// UsbStack::poll(), so the events stay in order.
//
// No Versaloon event comes during a command: the IN endpoint sends nothing,
// and the OUT endpoint reports no packet until the command ends. Its
// second buffer can hold one packet, but hw::UsbDevice reports it only
// when the reply is sent and the endpoint is armed again.
//
// The CMSIS-DAP personality runs a command outside the delivery of its OUT
// packet (usb/CmsisDapFunction.h). Its endpoints stay open during a
// command, and a packet of the host only waits in the processor. Thus
// setCommandEndpoints() lets the serial-only mode also give the Out and
// InComplete events of these endpoints. DAP_TransferAbort can then reach
// the processor during a command.

#include <cstddef>
#include <cstdint>
#include <peripherals/UsbDeviceTypes.h>
#include <utils/RingBuffer.h>

#include "usb/UsbDescriptors.h"

namespace usb {

template <std::size_t Size>
class SerialEventQueue final : public RingBufferBase<hw::usb::Event> {
   public:
   using Event = hw::usb::Event;

   // True: try_pop() gives only the serial port events at the front, and
   // the events of the command endpoints (setCommandEndpoints()).
   void setSerialOnly(bool serialOnly) {
      serialOnly_ = serialOnly;
   }

   // The serial-only mode also gives the Out and InComplete events of
   // these two endpoints. noEndpoint: none.
   void setCommandEndpoints(uint8_t outEndpoint, uint8_t inEndpoint) {
      commandOut_ = outEndpoint;
      commandIn_ = inEndpoint;
   }

   static constexpr uint8_t noEndpoint = 0xFF;

   static constexpr bool isSerialEvent(const Event &event) {
      return isPacketEvent(event) && (event.endpoint == cdcDataOutEndpoint || event.endpoint == cdcDataInEndpoint ||
                                      event.endpoint == cdcNotificationEndpoint);
   }

   // True: the serial-only mode gives this event.
   [[nodiscard]] bool isServiceEvent(const Event &event) const {
      return isSerialEvent(event) || (isPacketEvent(event) && event.endpoint != noEndpoint &&
                                      (event.endpoint == commandOut_ || event.endpoint == commandIn_));
   }

   // RingBufferBase. The interrupt calls try_push(), the main loop all
   // other functions. hw::UsbDevice::armOut() also calls try_push() in the
   // main loop, with the USB interrupt masked.
   [[nodiscard]] bool try_push(const Event &value) noexcept override {
      return ring_.try_push(value);
   }

   [[nodiscard]] bool try_pop(Event &out) noexcept override {
      if(serialOnly_) {
         // The interrupt only adds events at the end: the front stays.
         Event front{};
         if(!ring_.peek(front) || !isServiceEvent(front)) {
            return false;
         }
      }
      return ring_.try_pop(out);
   }

   [[nodiscard]] bool peek(Event &out) const noexcept override {
      return ring_.peek(out);
   }

   [[nodiscard]] std::size_t size() const noexcept override {
      return ring_.size();
   }

   [[nodiscard]] bool empty() const noexcept override {
      return ring_.empty();
   }

   [[nodiscard]] bool full() const noexcept override {
      return ring_.full();
   }

   void clear() noexcept override {
      ring_.clear();
   }

   private:
   static constexpr bool isPacketEvent(const Event &event) {
      return event.type == hw::usb::EventType::Out || event.type == hw::usb::EventType::InComplete;
   }

   RingBuffer<Event, Size> ring_;
   bool serialOnly_{ false };
   uint8_t commandOut_{ noEndpoint };
   uint8_t commandIn_{ noEndpoint };
};

} // namespace usb
