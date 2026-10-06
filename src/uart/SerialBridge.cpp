// The UART bridge of the firmware. See uart/SerialBridge.h.
#include "uart/SerialBridge.h"

#if !defined(VSL_DEBUG_CDC)

#include <optional>

#include "uart/UartBridge.h"
#include "uart/UartPort.h"
#include "usb/UsbStack.h"

namespace uart {

namespace {

using Bridge = UartBridge<UartPort, bridgeBufferSize>;

UartPort::Buffers buffers;
UartPort port{ buffers };
// Static storage: the serial port of the USB stack is known only in
// startBridge().
std::optional<Bridge> bridge;

} // namespace

void startBridge(usb::UsbStack &usbStack) {
   port.start();
   bridge.emplace(usbStack.serialPort(), port, buffers);
   bridge->start();
}

void pollBridge() {
   if(bridge) {
      bridge->poll();
   }
}

} // namespace uart

#endif
