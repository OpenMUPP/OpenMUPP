#pragma once

// MUPP_USB_SERIAL_PORT: a personality of the image has the CDC-ACM serial
// port (usb/UsbStack.h). The WCH-Link personality has none. In an image
// with the define, UsbStack::hasSerialPort() tells if the active
// personality has the port.

#if defined(MUPP_WITH_VERSALOON) || defined(MUPP_WITH_CMSIS_DAP)
#define MUPP_USB_SERIAL_PORT 1
#endif
