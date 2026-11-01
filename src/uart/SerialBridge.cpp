// SPDX-License-Identifier: GPL-3.0-or-later
// The UART bridge of the firmware. See uart/SerialBridge.h.
#include "uart/SerialBridge.h"

#if !defined(MUPP_DEBUG_CDC) && defined(MUPP_USB_SERIAL_PORT)

#include <optional>

#if defined(MUPP_WITH_CMSIS_DAP)
#include "uart/DapUartChannel.h"
#endif
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
#if defined(MUPP_WITH_CMSIS_DAP)
DapUartChannel<UartPort, bridgeBufferSize> dapChannel{ port, buffers };
#endif

} // namespace

void startBridge(usb::UsbStack &usbStack) {
   port.start();
   bridge.emplace(usbStack.serialPort(), port, buffers);
   bridge->start();
#if defined(MUPP_WITH_CMSIS_DAP)
   dapChannel.start(&*bridge);
#endif
}

void pollBridge() {
   if(bridge) {
      bridge->poll();
#if defined(MUPP_WITH_CMSIS_DAP)
      dapChannel.poll();
#endif
   }
}

#if defined(MUPP_WITH_CMSIS_DAP)
cmsisdap::DapUart *dapUart() {
   return &dapChannel;
}
#endif

} // namespace uart

#endif
