// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The UART bridge of the firmware: the CDC-ACM serial port to the UART of
// the board (USART1 on PA9/PA10 on the Mini, UART7 on PC2/PC3 on the YD
// board, UART7 on PA6/PA7 on OpenMUPP v1). Target only.
// uart/UartBridge.h has the data path, the flow control and the line
// coding rules. In an image with the CMSIS-DAP personality, the DAP_UART_*
// commands can take the UART from the bridge (uart/DapUartChannel.h).
//
// With MUPP_DEBUG_CDC the CDC port carries the debug log (plan section 9.2,
// debug/DebugLog.h). Then the firmware has no bridge and no DAP_UART_*
// commands: startBridge() and pollBridge() are empty, dapUart() is
// nullptr, and the UART stays off. The same holds for an image with only
// the WCH-Link personality: it has no serial port (usb/SerialPortConfig.h).
// In an image with more personalities, call startBridge() and pollBridge()
// only when UsbStack::hasSerialPort() is true.

#include "usb/SerialPortConfig.h"

namespace usb {
class UsbStack;
}

namespace cmsisdap {
class DapUart;
class UartWire;
} // namespace cmsisdap

namespace uart {

#if !defined(MUPP_DEBUG_CDC) && defined(MUPP_USB_SERIAL_PORT)
// Starts the UART at 115200 8N1 and connects the bridge to the serial port
// of usbStack. Call it once, before UsbStack::start().
void startBridge(usb::UsbStack &usbStack);

// Moves the serial data. Call it in each main loop iteration, after
// UsbStack::poll(), and in the serial service during a long command
// (protocol/Background.h), after UsbStack::pollSerial().
void pollBridge();

#if defined(MUPP_WITH_CMSIS_DAP)
// The UART for the DAP_UART_* commands of the CMSIS-DAP processor. Valid
// at any time, also before startBridge(). Until then it has no owner.
cmsisdap::DapUart *dapUart();

// The wire mode of the UART for ID_DAP_Vendor6 (uart/SingleWireLine.h).
// nullptr on a board without UART_TX_DIR. Use it after startBridge().
cmsisdap::UartWire *uartWire();
#endif
#else
inline void startBridge(usb::UsbStack & /*usbStack*/) {
}

inline void pollBridge() {
}

inline cmsisdap::DapUart *dapUart() {
   return nullptr;
}

inline cmsisdap::UartWire *uartWire() {
   return nullptr;
}
#endif

} // namespace uart
