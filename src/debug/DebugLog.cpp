// Debug log over the CDC-ACM port. See debug/DebugLog.h.
#include "debug/DebugLog.h"

#if defined(VSL_DEBUG_CDC)

#include <logging/Formatter.h>
#include <utils/Timekeeping.h>

#include "debug/LogSink.h"
#include "usb/UsbStack.h"

namespace debug {

logging::Logger appLog{ "app" };
logging::Logger usbLog{ "usb" };

namespace {

// Holds about 20 log lines while no terminal has the port open.
LogSink<1024> sink;

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
bool wasTerminalOpen = false;

} // namespace

void startLog() {
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
   auto &port = usbStack.serialPort();
   const bool terminalOpen = port.isTerminalOpen();
   if(terminalOpen != wasTerminalOpen) {
      wasTerminalOpen = terminalOpen;
      LOG_INFO(usbLog, terminalOpen ? "terminal open" : "terminal closed");
   }
   sink.drainTo(port);
}

} // namespace debug

#endif
