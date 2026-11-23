// SPDX-License-Identifier: GPL-3.0-or-later
// Debug log over the CDC-ACM port and the debug UART. See debug/DebugLog.h.
#include "debug/DebugLog.h"

#if defined(MUPP_DEBUG_CDC) || defined(MUPP_DEBUG_UART)

#include <logging/Formatter.h>
#include <utils/Timekeeping.h>

#include "debug/DebugUart.h"
#include "debug/LogSink.h"
#include "usb/UsbStack.h"

namespace debug {

logging::Logger appLog{ "app" };
logging::Logger usbLog{ "usb" };

namespace {

#if defined(MUPP_DEBUG_CDC)
// Holds about 20 log lines while no terminal has the port open.
LogSink<1024> cdcSink;
#endif

#if defined(MUPP_DEBUG_UART)
LogSink<1024> uartSink;
DebugUart uartPort;
#endif

#if defined(MUPP_DEBUG_CDC) && defined(MUPP_DEBUG_UART)
TeeSink sink{ cdcSink, uartSink };
#elif defined(MUPP_DEBUG_CDC)
LogSink<1024> &sink = cdcSink;
#else
LogSink<1024> &sink = uartSink;
#endif

// "<seconds>.<milliseconds> [topic][LEVEL] text\r\n". One SysTick is 1 ms.
class UptimeFormatter final : public logging::Formatter {
   public:
   art::expected<size_t, Writable::Error> formatPrefix(Writable &out, logging::Level level, const char *topic) override {
      const auto ms = static_cast<uint64_t>(getUptimeTicks());
      return out.printf("%u.%03u [%s][%s] ", static_cast<unsigned>(ms / 1000), static_cast<unsigned>(ms % 1000), topic,
                        logging::Logger::levelStrings[static_cast<int>(level)]);
   }

   art::expected<size_t, Writable::Error> formatSuffix(Writable &out) override {
      return out.println();
   }
};

UptimeFormatter formatter;

bool wasConfigured = false;
#if defined(MUPP_DEBUG_CDC)
bool wasTerminalOpen = false;
#endif

} // namespace

void startLog() {
#if defined(MUPP_DEBUG_UART)
   uartPort.start();
#endif
   auto &manager = LogManager::getInstance();
   manager.set_formatter(&formatter);
   manager.set_sink(&sink);
}

void pollLog(usb::UsbStack &usbStack) {
   const bool configured = usbStack.isConfigured();
   if(configured != wasConfigured) {
      wasConfigured = configured;
      LOG_INFO(usbLog, configured ? "configured" : "not configured");
   }
#if defined(MUPP_DEBUG_CDC)
   if(usbStack.hasSerialPort()) {
      auto &port = usbStack.serialPort();
      const bool terminalOpen = port.isTerminalOpen();
      if(terminalOpen != wasTerminalOpen) {
         wasTerminalOpen = terminalOpen;
         LOG_INFO(usbLog, terminalOpen ? "terminal open" : "terminal closed");
      }
      cdcSink.drainTo(port);
   }
#endif
#if defined(MUPP_DEBUG_UART)
   uartSink.drainTo(uartPort);
#endif
}

} // namespace debug

#endif
