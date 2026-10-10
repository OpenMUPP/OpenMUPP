// Versaloon next generation firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and on
// each USB interrupt. It runs the USB device, the UART bridge
// (uart/SerialBridge.h) or, in a VSL_DEBUG_CDC build, the debug log
// (debug/DebugLog.h), and drives the heartbeat on the
// state LED and the USB state on the USB LED. After VERSALOON_FW_UPDATE it
// enters versaloon-bootloader (boot/BootloaderEntry.h). The USB_TO_XXX
// commands of OpenOCD go to the USB_TO_ALL dispatcher (protocol/UsbToAll.h),
// with the GPIO block (protocol/GpioBlock.h) on the debug connector pins
// the JTAG_RAW block (protocol/JtagRawBlock.h) on the JTAG port of the
// board (board/BoardJtag.h: SPI fast path or bit-bang), and the SWD block (protocol/SwdBlock.h) on the
// bit-bang SWD engine (swd/SwdBitBang.h). The POWER block
// (protocol/PowerBlock.h) switches the target supply, and its poll() in the
// main loop switches it off when the rail drops. GET_TVCC replies the
// voltage of its Vref sense. During a long JTAG or SWD command, the
// engines call the serial service (protocol/Background.h): it runs the
// serial port of the USB device and the UART bridge, or the debug log.
#include <cmsis_device.h>
#include <delaybusywait.h>
#include <utils/Timekeeping.h>

#include <array>

#include "Hardware.h"
#include "Heartbeat.h"
#include "UptimeMicros.h"
#include "Version.h"
#include "board/Board.h"
#include "board/BoardGpio.h"
#include "board/BoardSwd.h"
#include "board/Leds.h"
#include "debug/DebugLog.h"
#include "protocol/Background.h"
#include "protocol/GpioBlock.h"
#include "uart/SerialBridge.h"
#include "usb/UsbStack.h"

#if defined(VSL_PROTOCOL_CMSIS_DAP)
#include <peripherals/Uid.h>

#include "protocol/cmsisdap/BitBangDapSwd.h"
#include "protocol/cmsisdap/DapProcessor.h"
#else
#include "board/BoardJtag.h"
#include "board/BoardPower.h"
#include "boot/Bootloader.h"
#include "boot/BootloaderEntry.h"
#include "protocol/JtagRawBlock.h"
#include "protocol/PowerBlock.h"
#include "protocol/SwdBlock.h"
#include "protocol/UsbToAll.h"
#endif

namespace {

// The serial port during a long command. It runs only the serial port
// events of the USB device (UsbStack::pollSerial()), so no command starts
// and the host stays connected for the running command.
class SerialService final : public protocol::Background {
   public:
   void service() override;
};

SerialService serialService;

#if defined(VSL_PROTOCOL_CMSIS_DAP)

// The CMSIS-DAP strings are NUL-terminated and must stay valid, so the
// processor keeps a pointer to the serial number text.
constexpr const char *dapVendor = "Versaloon";
constexpr const char *dapProduct = "Versaloon CMSIS-DAP";

std::array<char, 2 * hw::Uid::SizeBytes + 1> dapSerialNumber{};

// Time of the processor. It is the uptime in microseconds (1 ms per
// SysTick) and wraps at 2^32 as the processor needs. getUptimeMilliTicks()
// of system-stm32 can go back by 1 ms when the SysTick counter wraps
// with interrupts off, so the registers are read here (UptimeMicros.h).
class DapPlatform final : public cmsisdap::Platform {
   public:
   uint32_t nowUs() override {
      const uint32_t primask = __get_PRIMASK();
      __disable_irq();
      const auto ticks = static_cast<uint64_t>(getUptimeTicks());
      const uint32_t value = SysTick->VAL;
      const bool tickPending = (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk) != 0;
      __set_PRIMASK(primask);
      return uptimeMicroseconds(ticks, SysTick->LOAD + 1, value, tickPending);
   }
   void delayUs(uint32_t microseconds) override {
      // The pin wait of DAP_SWJ_Pins is up to 3 s
      delayuS(microseconds);
   }
   // The LEDs show the heartbeat and the USB state, as in the Versaloon
   // personality. DAP_HostStatus changes nothing.
   void setLed(cmsisdap::Led /*led*/, bool /*on*/) override {
   }
};

DapPlatform dapPlatform;
board::BoardGpio boardGpio;
board::SwdPins swdPins;
board::BoardSwd boardSwd{ swdPins, &serialService };
cmsisdap::BitBangDapSwd<board::BoardSwd> dapSwd{ boardSwd };
cmsisdap::Processor dapProcessor{ dapSwd, boardGpio, dapPlatform,
                                  cmsisdap::DeviceInfo{
                                  .vendor = dapVendor,
                                  .product = dapProduct,
                                  .serialNumber = dapSerialNumber.data(),
                                  .firmwareVersion = versionString,
                                  } };
usb::UsbStack usbStack{ dapProcessor };

#else

// The DELAY block busy-waits: the main loop stops for the delay, up to
// 32.767 s. The host waits for the reply in this time.
class BusyWaitDelay final : public protocol::Delay {
   public:
   void microseconds(uint16_t us) override {
      delayuS(us);
   }
   void milliseconds(uint16_t ms) override {
      delaymS(ms);
   }
};

BusyWaitDelay busyWaitDelay;

protocol::UsbToAll usbToAll{ busyWaitDelay };
board::BoardGpio boardGpio;
protocol::GpioBlock gpioBlock{ boardGpio };
board::JtagPins jtagPins;
board::BoardJtag boardJtag{ jtagPins, &serialService };
protocol::JtagRawBlock jtagRawBlock{ boardJtag };
board::SwdPins swdPins;
board::BoardSwd boardSwd{ swdPins, &serialService };
protocol::SwdBlock swdBlock{ boardSwd };
board::BoardPower boardPower;
protocol::PowerBlock powerBlock{ boardPower, busyWaitDelay };
boot::BootloaderEntry bootloaderEntry;
usb::UsbStack usbStack{ boot::hasBootloader ? &bootloaderEntry : nullptr, usbToAll };

#endif

// A command runs only after usbStack.start(), so the USB device and the
// bridge run.
void SerialService::service() {
   usbStack.pollSerial();
   debug::pollLog(usbStack);
   uart::pollBridge();
}

#if defined(VSL_PROTOCOL_CMSIS_DAP)

// The chip UID as the 24 hexadecimal digits of the USB serial number
// string (usb/UsbDescriptors.h: SerialNumber), so the host tools see one
// serial number in the USB string and in DAP_Info.
void setDapSerialNumber() {
   std::array<uint8_t, hw::Uid::SizeBytes> uid{};
   hw::Uid::read(uid);
   constexpr char digits[] = "0123456789ABCDEF";
   for(std::size_t i = 0; i < uid.size(); ++i) {
      dapSerialNumber[2 * i] = digits[uid[i] >> 4];
      dapSerialNumber[2 * i + 1] = digits[uid[i] & 0x0F];
   }
}

// Before the first command
void startProtocol() {
   setDapSerialNumber();
}

// The processor has no periodic work: the USB function runs it.
void beforeUsbPoll(uint64_t /*nowMs*/) {
}

void afterUsbPoll(uint64_t /*nowMs*/) {
}

#else

// Adds the blocks of USB_TO_ALL before the first command
void startProtocol() {
   if(!usbToAll.add(gpioBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the GPIO block");
   }
   if(!usbToAll.add(jtagRawBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the JTAG_RAW block");
   }
   if(!usbToAll.add(swdBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the SWD block");
   }
   boardPower.init();
   if(!usbToAll.add(powerBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the POWER block");
   }
   usbToAll.setTargetVoltage(&powerBlock);
}

void beforeUsbPoll(uint64_t nowMs) {
   if(bootloaderEntry.isDue(nowMs)) {
      boot::enterBootloader();
   }
}

void afterUsbPoll(uint64_t nowMs) {
   powerBlock.poll(nowMs);
}

#endif

} // namespace

int main() {
   const bool clockFromCrystal = hardware::isClockFromCrystal();
   const Heartbeat heartbeat{ clockFromCrystal ? Heartbeat::normal : Heartbeat::clockFault };

   debug::startLog();
   // The board names are string literals, so name.data() ends with a NUL.
   LOG_INFO(debug::appLog, "versaloon-next-gen %s, board %s", versionString, board::current.name.data());
   startProtocol();

   // USB needs the 48 MHz clock from the PLL. Without the crystal the
   // device stays disconnected, and the UART bridge is off.
   if(clockFromCrystal) {
      uart::startBridge(usbStack);
      usbStack.start();
   }

   for(;;) {
      // One SysTick is 1 ms (initialize_hardware.cpp)
      const auto nowMs = static_cast<uint64_t>(getUptimeTicks());
      beforeUsbPoll(nowMs);
      if(clockFromCrystal) {
         usbStack.poll();
         debug::pollLog(usbStack);
         uart::pollBridge();
      }
      afterUsbPoll(nowMs);
      board::setLed(board::Led::State, heartbeat.isOn(nowMs));
      board::setLed(board::Led::Usb, usbStack.isConfigured());
      __WFI();
   }
}
