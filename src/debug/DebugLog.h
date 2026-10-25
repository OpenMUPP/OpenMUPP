#pragma once

// Debug log over the CDC-ACM port (plan section 9.2). Target only.
//
// The CMake option VSL_DEBUG_CDC (ON by default in a Debug build) defines
// VSL_DEBUG_CDC. Then the libArT logger writes into a LogSink (debug/
// LogSink.h), and pollLog() sends the log to the one CDC-ACM port of the
// device. The USB descriptors are the same in both builds.
//
// Without the option, CMake defines LOGGING_DISABLED: the LOG_* macros
// expand to nothing, the loggers do not exist and startLog()/pollLog() are
// empty. A release build carries no log code.
//
// Contract for the UART bridge (B5): with VSL_DEBUG_CDC the CDC port
// belongs to the log. Build and start the bridge only when
// debug::cdcLog is false. USART1 then stays unused, and the device ignores
// the data that the host sends to the CDC port.
//
// Call pollLog() only when UsbStack::hasSerialPort() is true: the
// WCH-Link personality has no CDC port. The log lines of a start without
// the port stay in the sink until it is full, and the newer ones are lost.
//
// Use: LOG_INFO(debug::appLog, "format", args...). Log only from the main
// loop, never from an interrupt handler (see debug/LogSink.h).

#include <logging/Logger.h>

#include "usb/SerialPortConfig.h"

#if defined(VSL_DEBUG_CDC) && !defined(VSL_USB_SERIAL_PORT)
#error "The image has no CDC port for the debug log: the WCH-Link personality has none"
#endif

namespace usb {
class UsbStack;
}

namespace debug {

#if defined(VSL_DEBUG_CDC)
inline constexpr bool cdcLog = true;

// Log topics
extern logging::Logger appLog;
extern logging::Logger usbLog;

// Connects the loggers to the log sink. Call it once, before the first
// log line.
void startLog();

// Sends the buffered log to the CDC port and logs USB state changes. Call
// it in each main loop iteration, after UsbStack::poll(), and in the
// serial service during a long command (protocol/Background.h).
void pollLog(usb::UsbStack &usbStack);
#else
inline constexpr bool cdcLog = false;

inline void startLog() {
}

inline void pollLog(usb::UsbStack & /*usbStack*/) {
}
#endif

} // namespace debug
