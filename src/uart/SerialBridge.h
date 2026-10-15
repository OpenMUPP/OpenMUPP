#pragma once

// The UART bridge of the firmware: the CDC-ACM serial port to the UART of
// the board (USART1 on PA9/PA10 on the Mini and the OtherUse). Target only.
// uart/UartBridge.h has the data path, the flow control and the line
// coding rules.
//
// With VSL_DEBUG_CDC the CDC port carries the debug log (plan section 9.2,
// debug/DebugLog.h). Then the firmware has no bridge: startBridge() and
// pollBridge() are empty, and the UART stays off. The same holds for the
// WCH-Link personality (VSL_WITH_WCH_LINK): it has no serial port.

namespace usb {
class UsbStack;
}

namespace uart {

#if !defined(VSL_DEBUG_CDC) && !defined(VSL_WITH_WCH_LINK)
// Starts the UART at 115200 8N1 and connects the bridge to the serial port
// of usbStack. Call it once, before UsbStack::start().
void startBridge(usb::UsbStack &usbStack);

// Moves the serial data. Call it in each main loop iteration, after
// UsbStack::poll(), and in the serial service during a long command
// (protocol/Background.h), after UsbStack::pollSerial().
void pollBridge();
#else
inline void startBridge(usb::UsbStack & /*usbStack*/) {
}

inline void pollBridge() {
}
#endif

} // namespace uart
