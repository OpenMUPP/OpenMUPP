// versaloon-NG firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and on
// each USB interrupt. It runs the USB device, the UART bridge
// (uart/SerialBridge.h) or, in a VSL_DEBUG_CDC build, the debug log
// (debug/DebugLog.h), and drives the LEDs: the target power on the power
// LED, the command activity on the state LED (the probe error pattern
// after a JTAG or SWD error, the clock fault pattern when the crystal
// fails) and the USB state on the USB LED. After VERSALOON_FW_UPDATE it
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
// A CMSIS-DAP image runs the CMSIS-DAP processor
// (protocol/cmsisdap/DapProcessor.h) instead of USB_TO_ALL. Its DAP_UART_*
// commands can take the UART from the bridge (uart/DapUartChannel.h). A
// dual image has both: the stored personality (personality/PersonalityStore.h)
// selects one at the start. A switch command, or a key that is down for 3 s
// (then the key release), stores the next personality and resets (personality/PersonalitySwitch.h).
// A wch-link image runs the WCH-Link processor (protocol/wchlink/WchLinkProcessor.h)
// on the RISC-V debug wire engine (board/BoardRvSwd.h). It has no serial
// port: no UART bridge and no debug log.
//
// Each personality has one group below, inside its VSL_WITH_<P> define:
// its objects, its start, its work before and after the USB poll, and
// its probe error count. Only the group of the active personality runs.
// The constructors only keep references, so no group touches the hardware
// before main(). The active USB function alone gets commands, so only the
// active personality configures its pins, timers and DMA channels: the
// SWD and JTAG pins and the JTAG fast path (DMA1 channels 3 to 5) for
// Versaloon and CMSIS-DAP, the RISC-V debug wire with the one-wire timer
// and two DMA1 channels (board::oneWireTimerOf()) for WCH-Link. A
// personality switch resets the MCU, so nothing is set back.
#include <cmsis_device.h>
#include <delaybusywait.h>
#include <utils/Timekeeping.h>

#include <array>

#include "ActivityLed.h"
#include "ErrorLed.h"
#include "Hardware.h"
#include "Heartbeat.h"
#include "TargetPowerLed.h"
#include "UptimeMicros.h"
#include "Version.h"
#include "board/Board.h"
#include "board/BoardGpio.h"
#include "board/BoardJtag.h"
#include "board/BoardPower.h"
#include "board/BoardSwd.h"
#include "board/Leds.h"
#include "debug/DebugLog.h"
#include "protocol/Background.h"
#include "protocol/GpioBlock.h"
#include "uart/SerialBridge.h"
#include "usb/UsbStack.h"

#if defined(VSL_WITH_WCH_LINK)
#include "board/BoardRvSwd.h"
#include "protocol/wchlink/WchLinkProcessor.h"
#endif
#if defined(VSL_WITH_CMSIS_DAP)
#include <peripherals/Uid.h>

#include "protocol/cmsisdap/BitBangDapSwd.h"
#include "protocol/cmsisdap/DapProcessor.h"
#endif
#if defined(VSL_WITH_VERSALOON)
#include "boot/Bootloader.h"
#include "boot/BootloaderEntry.h"
#include "protocol/JtagRawBlock.h"
#include "protocol/PowerBlock.h"
#include "protocol/SwdBlock.h"
#include "protocol/UsbToAll.h"
#endif
#if defined(VSL_DUAL_PERSONALITY)
#include "board/BoardPins.h"
#include "board/SettingsFlashPage.h"
#include "personality/KeyGesture.h"
#include "personality/PersonalityStore.h"
#include "personality/PersonalitySwitch.h"
#endif

namespace {

using personality::Personality;

#if defined(VSL_WITH_VERSALOON) || defined(VSL_WITH_CMSIS_DAP)

// The SWD and JTAG ports of the Versaloon and CMSIS-DAP personalities

// The serial port during a long command. It runs only the serial port
// events of the USB device (UsbStack::pollSerial()), so no command starts
// and the host stays connected for the running command.
class SerialService final : public protocol::Background {
   public:
   void service() override;
};

SerialService serialService;

board::BoardGpio boardGpio;
board::SwdPins swdPins;
board::BoardSwd boardSwd{ swdPins, &serialService };
board::JtagPins jtagPins;
board::BoardJtag boardJtag{ jtagPins, &serialService };

#endif

#if defined(VSL_DUAL_PERSONALITY)

// The personality when the settings page has none
#if defined(VSL_DEFAULT_CMSIS_DAP)
constexpr Personality defaultPersonality = Personality::CmsisDap;
#else
constexpr Personality defaultPersonality = Personality::Versaloon;
#endif

Personality loadPersonality() {
   board::SettingsFlashPage page;
   return personality::PersonalityStore{ page }.load().value_or(defaultPersonality);
}

// The personality of this start. Before the USB stack, which takes it.
const Personality activePersonality = loadPersonality();
personality::PersonalitySwitch personalitySwitch{ activePersonality };
personality::KeyGesture keyGesture;

#else

// The one personality of the image
#if defined(VSL_WITH_WCH_LINK)
constexpr Personality activePersonality = Personality::WchLink;
#elif defined(VSL_WITH_CMSIS_DAP)
constexpr Personality activePersonality = Personality::CmsisDap;
#else
constexpr Personality activePersonality = Personality::Versaloon;
#endif

#endif

// True when the personality runs in this start. In an image with one
// personality it is a constant, so the compiler drops the other groups.
bool isActive(Personality personality) {
   return activePersonality == personality;
}

// The Vref sense and the target switch. The target power LED reads the
// sense in every personality.
board::BoardPower boardPower;

#if defined(VSL_WITH_WCH_LINK)

// --- WCH-Link ---------------------------------------------------------
// The processor runs in the main loop, outside the USB event delivery. The
// target gets no Background: the personality has no serial port to service
// during a long command. rvSwdPins.init() sets up the one-wire timer and
// its DMA channels when the host attaches the target (wchProcessor), so
// only in a WCH-Link start. The personality has no start, no poll work and no error count.

class WchPlatform final : public wchlink::Platform {
   public:
   void delayUs(uint32_t microseconds) override {
      delayuS(microseconds);
   }
};

WchPlatform wchPlatform;
board::RvSwdPins rvSwdPins;
wchlink::RvSwdTarget<board::RvSwdPins> wchTarget{ rvSwdPins };
wchlink::Processor wchProcessor{ wchTarget, wchPlatform };

#endif

#if defined(VSL_WITH_CMSIS_DAP)

// --- CMSIS-DAP --------------------------------------------------------
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
   // The LEDs show the target power, the activity and the USB state, as
   // in the Versaloon personality. DAP_HostStatus changes nothing.
   void setLed(cmsisdap::Led /*led*/, bool /*on*/) override {
   }
};

DapPlatform dapPlatform;
cmsisdap::BitBangDapSwd<board::BoardSwd> dapSwd{ boardSwd };
cmsisdap::Processor dapProcessor{ dapSwd,
                                  boardGpio,
                                  dapPlatform,
                                  cmsisdap::DeviceInfo{
                                  .vendor = dapVendor,
                                  .product = dapProduct,
                                  .serialNumber = dapSerialNumber.data(),
                                  .firmwareVersion = versionString,
                                  },
                                  &boardJtag,
#if defined(VSL_DUAL_PERSONALITY)
                                  &personalitySwitch,
#else
                                  nullptr,
#endif
                                  uart::dapUart() };

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

// The CMSIS-DAP processor has no periodic work: UsbStack::poll() runs it.
void startDap() {
   setDapSerialNumber();
}

#endif

#if defined(VSL_WITH_VERSALOON)

// --- Versaloon --------------------------------------------------------
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
protocol::GpioBlock gpioBlock{ boardGpio };
protocol::JtagRawBlock jtagRawBlock{ boardJtag };
protocol::SwdBlock swdBlock{ boardSwd };
protocol::PowerBlock powerBlock{ boardPower, busyWaitDelay };
boot::BootloaderEntry bootloaderEntry;

// FW_UPDATE enters the bootloader only in the app-8k image, behind
// versaloon-bootloader. CMake defines VSL_HAS_BOOTLOADER_ENTRY for this file
// of that image only (vsl_add_app_image() in CMakeLists.txt). The other
// sources are shared by all images and compiled without the define, so the
// value is here and not in a shared header.
#if defined(VSL_HAS_BOOTLOADER_ENTRY)
constexpr bool hasBootloaderEntry = true;
#else
constexpr bool hasBootloaderEntry = false;
#endif

const usb::UsbStack::VersaloonDeps versaloonDeps{
   .bootloader = hasBootloaderEntry ? &bootloaderEntry : nullptr,
   .usbToXxx = &usbToAll,
};

// Adds the blocks of USB_TO_ALL before the first command
void startVersaloon() {
   if(!usbToAll.add(gpioBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the GPIO block");
   }
   if(!usbToAll.add(jtagRawBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the JTAG_RAW block");
   }
   if(!usbToAll.add(swdBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the SWD block");
   }
   if(!usbToAll.add(powerBlock)) {
      LOG_ERROR(debug::appLog, "USB_TO_ALL: cannot add the POWER block");
   }
   usbToAll.setTargetVoltage(&powerBlock);
}

void beforeUsbPollVersaloon(uint64_t nowMs) {
   if(bootloaderEntry.isDue(nowMs)) {
      boot::enterBootloader();
   }
}

void afterUsbPollVersaloon(uint64_t nowMs) {
   powerBlock.poll(nowMs);
}

#endif

usb::UsbStack usbStack{ activePersonality, usb::UsbStack::Functions{
#if defined(VSL_WITH_VERSALOON)
                                           .versaloon = &versaloonDeps,
#else
                                           .versaloon = nullptr,
#endif
#if defined(VSL_WITH_CMSIS_DAP)
                                           .dap = &dapProcessor,
#else
                                           .dap = nullptr,
#endif
#if defined(VSL_WITH_WCH_LINK)
                                           .wch = &wchProcessor,
#else
                                           .wch = nullptr,
#endif
#if defined(VSL_DUAL_PERSONALITY)
                                           .personalitySwitch = &personalitySwitch,
#else
                                           .personalitySwitch = nullptr,
#endif
                                           } };

#if defined(VSL_WITH_VERSALOON) || defined(VSL_WITH_CMSIS_DAP)
// A command runs only after usbStack.start(), so the USB device and the
// bridge run. Only the Versaloon and CMSIS-DAP engines call it. As in the
// main loop, the log and the bridge run only with the serial port.
void SerialService::service() {
   usbStack.pollSerial();
   if(usbStack.hasSerialPort()) {
      debug::pollLog(usbStack);
      uart::pollBridge();
   }
}
#endif

#if defined(VSL_DUAL_PERSONALITY)

// --- Personality switch -----------------------------------------------
// Stores the personality and resets. The USB device then enumerates with
// the descriptors of that personality. When the store fails, the reset
// keeps the active personality.
[[noreturn]] void switchPersonality(Personality target) {
   LOG_INFO(debug::appLog, "Switch to the %s personality", personality::name(target));
   usbStack.disconnect();
   board::SettingsFlashPage page;
   static_cast<void>(personality::PersonalityStore{ page }.store(target));
   NVIC_SystemReset();
   for(;;) {
   }
}

// The key gesture and the switch commands. The key gesture gives the
// request when the key has been down for 3 s, and the LEDs flash. The
// reset waits for the release of the key, with a 50 ms debounce: a key
// that is down at the reset starts the bootloader.
void pollPersonality(uint64_t nowMs) {
   bool keyDown = false;
   if(const auto key = board::current.key) {
      keyDown = board::isAsserted(*key);
      if(keyGesture.update(keyDown, nowMs)) {
         personalitySwitch.request(personality::next(personalitySwitch.active()), personality::PersonalitySwitch::flashDelayMs);
      }
   }
   // The reset waits for the release of the key (PersonalitySwitch.h)
   if(personalitySwitch.isDue(nowMs, keyDown)) {
      switchPersonality(*personalitySwitch.requested());
   }
}

#endif

// --- The active personality -------------------------------------------
// Before the first command
void startProtocol() {
   boardPower.init();
#if defined(VSL_WITH_CMSIS_DAP)
   if(isActive(Personality::CmsisDap)) {
      startDap();
   }
#endif
#if defined(VSL_WITH_VERSALOON)
   if(isActive(Personality::Versaloon)) {
      startVersaloon();
   }
#endif
}

void beforeUsbPoll([[maybe_unused]] uint64_t nowMs) {
#if defined(VSL_WITH_VERSALOON)
   if(isActive(Personality::Versaloon)) {
      beforeUsbPollVersaloon(nowMs);
   }
#endif
#if defined(VSL_DUAL_PERSONALITY)
   pollPersonality(nowMs);
#endif
}

void afterUsbPoll([[maybe_unused]] uint64_t nowMs) {
#if defined(VSL_WITH_VERSALOON)
   if(isActive(Personality::Versaloon)) {
      afterUsbPollVersaloon(nowMs);
   }
#endif
}

// Number of JTAG and SWD errors of the active personality, for the error
// LED. It wraps at 2^32: only the difference between two values counts.
uint32_t probeErrorCount() {
#if defined(VSL_WITH_VERSALOON)
   if(isActive(Personality::Versaloon)) {
      return swdBlock.errorCount();
   }
#endif
#if defined(VSL_WITH_CMSIS_DAP)
   if(isActive(Personality::CmsisDap)) {
      return dapProcessor.errorCount();
   }
#endif
   return 0;
}

constexpr Heartbeat clockFaultBlink{ Heartbeat::clockFault };
ActivityLed activityLed;
ErrorLed errorLed;
TargetPowerLed targetPowerLed;

// Drives the LEDs (docs/boards.md, "LEDs"). Call it in each main loop
// iteration, after the USB poll.
void updateLeds(uint64_t nowMs, bool clockFromCrystal) {
#if defined(VSL_DUAL_PERSONALITY)
   // The key gesture flashes all LEDs before the switch.
   if(const auto flash = personalitySwitch.indication(nowMs)) {
      board::setLed(board::Led::Power, *flash);
      board::setLed(board::Led::State, *flash);
      board::setLed(board::Led::Usb, *flash);
      return;
   }
#endif
   if(boardPower.hasSense() && targetPowerLed.isDue(nowMs)) {
      targetPowerLed.update(nowMs, boardPower.senseMillivolts());
   }
   activityLed.update(nowMs, usbStack.commandCount());
   errorLed.update(nowMs, probeErrorCount());
   board::setLed(board::Led::Power, targetPowerLed.isOn());
   // A probe error replaces the activity for some time (ErrorLed.h).
   const bool stateOn = errorLed.isActive() ? errorLed.isOn() : activityLed.isOn();
   board::setLed(board::Led::State, clockFromCrystal ? stateOn : clockFaultBlink.isOn(nowMs));
   board::setLed(board::Led::Usb, usbStack.isConfigured());
}

} // namespace

int main() {
   const bool clockFromCrystal = hardware::isClockFromCrystal();

   debug::startLog();
   // The board names are string literals, so name.data() ends with a NUL.
   LOG_INFO(debug::appLog, "versaloon-ng %s, board %s", versionString, board::current.name.data());
   startProtocol();

   // USB needs the 48 MHz clock from the PLL. Without the crystal the
   // device stays disconnected, and the UART bridge is off.
   if(clockFromCrystal) {
      if(usbStack.hasSerialPort()) {
         uart::startBridge(usbStack);
      }
      usbStack.start();
   }

   for(;;) {
      // One SysTick is 1 ms (initialize_hardware.cpp)
      const auto nowMs = static_cast<uint64_t>(getUptimeTicks());
      beforeUsbPoll(nowMs);
      if(clockFromCrystal) {
         usbStack.poll();
         if(usbStack.hasSerialPort()) {
            debug::pollLog(usbStack);
            uart::pollBridge();
         }
      }
      afterUsbPoll(nowMs);
      updateLeds(nowMs, clockFromCrystal);
      // Sleep only when no USB event waits. An event that came after the
      // USB poll has no pending interrupt to end the WFI: the interrupt
      // handler already ran, or the main loop queued the event itself (the
      // double-buffered OUT endpoint reports a waiting packet when it is
      // armed again). With the interrupts masked, an interrupt still ends
      // the WFI, and its handler runs after __enable_irq().
      __disable_irq();
      if(!clockFromCrystal || !usbStack.hasPendingEvents()) {
         __WFI();
      }
      __enable_irq();
   }
}
