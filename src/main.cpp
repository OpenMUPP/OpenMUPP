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
#include "boot/Bootloader.h"
#include "boot/BootloaderEntry.h"
#include "debug/DebugLog.h"
#include "protocol/Background.h"
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
#if defined(VSL_CMSIS_DAP_RV_BRIDGE)
#include "board/BoardRvSwd.h"
#include "protocol/cmsisdap/RvBridge.h"
#include "protocol/cmsisdap/RvDtmTap.h"
#endif
#endif
#if defined(VSL_WITH_VERSALOON)
#include "protocol/versaloon/GpioBlock.h"
#include "protocol/versaloon/JtagRawBlock.h"
#include "protocol/versaloon/PowerBlock.h"
#include "protocol/versaloon/SwdBlock.h"
#include "protocol/versaloon/UsbToAll.h"
#endif
#if defined(VSL_MULTI_PERSONALITY)
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

#if defined(VSL_MULTI_PERSONALITY)

Personality loadPersonality() {
   board::SettingsFlashPage page;
   return personality::PersonalityStore{ page }.load().value_or(personality::defaultPersonality);
}

// The personality of this start. Before the USB stack, which takes it.
const Personality activePersonality = loadPersonality();
personality::PersonalitySwitch personalitySwitch{ activePersonality };
personality::KeyGesture keyGesture;

#else

// The one personality of the image
constexpr Personality activePersonality = personality::all[0];

#endif

// True when the personality runs in this start. In an image with one
// personality it is a constant, so the compiler drops the other groups.
// The constructors only keep references, so no group touches the hardware
// before main(). Only the active personality configures its pins, timers
// and DMA channels. A personality switch resets the MCU, so nothing is set
// back.
bool isActive(Personality personality) {
   return activePersonality == personality;
}

// The Vref sense and the target switch. The target power LED reads the
// sense in every personality.
board::BoardPower boardPower;

// The bootloader entry of FW_UPDATE and of the bootloader code of the switch
// commands, in every personality. On the STM32F1 only the app-8k image
// enters the bootloader, behind versaloon-bootloader. On the STM32H5 the
// standalone image enters the ROM bootloader. CMake defines
// VSL_HAS_BOOTLOADER_ENTRY for this file of these images only
// (vsl_add_app_image() in CMakeLists.txt). The other sources are shared by
// all images and compiled without the define, so the value is here and not
// in a shared header.
#if defined(VSL_HAS_BOOTLOADER_ENTRY)
constexpr bool hasBootloaderEntry = true;
#else
constexpr bool hasBootloaderEntry = false;
#endif
boot::BootloaderEntry bootloaderEntry;
boot::BootloaderEntry *const bootloader = hasBootloaderEntry ? &bootloaderEntry : nullptr;

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
#if defined(VSL_CMSIS_DAP_RV_BRIDGE)
// The RISC-V bridge: ID_DAP_Vendor1 switches the JTAG port from the board
// to the emulated RISC-V DTM on the debug wire engine.
board::RvSwdPins dapRvSwdPins;
wchlink::RvSwdTarget<board::RvSwdPins> dapRvTarget{ dapRvSwdPins, &serialService };
cmsisdap::RvDtmTap dapRvTap{ dapRvTarget };
cmsisdap::RvBridge dapRvBridge{ boardJtag, dapRvTap };
#endif
cmsisdap::Processor dapProcessor{ dapSwd,
                                  boardGpio,
                                  dapPlatform,
                                  cmsisdap::DeviceInfo{
                                  .vendor = dapVendor,
                                  .product = dapProduct,
                                  .serialNumber = dapSerialNumber.data(),
                                  .firmwareVersion = versionString,
                                  },
#if defined(VSL_CMSIS_DAP_RV_BRIDGE)
                                  &dapRvBridge,
#else
                                  &boardJtag,
#endif
#if defined(VSL_MULTI_PERSONALITY)
                                  &personalitySwitch,
#else
                                  nullptr,
#endif
                                  uart::dapUart(),
                                  bootloader,
#if defined(VSL_CMSIS_DAP_RV_BRIDGE)
                                  &dapRvBridge };
#else
                                  nullptr };
#endif

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
const usb::UsbStack::VersaloonDeps versaloonDeps{
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

// Switches the target supply off when the rail drops
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
#if defined(VSL_MULTI_PERSONALITY)
                                           .personalitySwitch = &personalitySwitch,
#else
                                           .personalitySwitch = nullptr,
#endif
                                           .bootloader = bootloader,
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

#if defined(VSL_MULTI_PERSONALITY)

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
   // Only the images with the bootloader entry link boot::enterBootloader()
   if constexpr(hasBootloaderEntry) {
      if(bootloaderEntry.isDue(nowMs)) {
         boot::enterBootloader();
      }
   }
#if defined(VSL_MULTI_PERSONALITY)
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
#if defined(VSL_MULTI_PERSONALITY)
   // The key gesture flashes all LEDs before the switch.
   if(const auto flash = personalitySwitch.indication(nowMs)) {
      board::setLed(board::Led::Power, *flash);
      board::showState(*flash ? board::StateShow::Activity : board::StateShow::Idle);
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
   using board::StateShow;
   StateShow show = activityLed.isOn() ? StateShow::Activity : StateShow::Idle;
   if(!clockFromCrystal) {
      show = clockFaultBlink.isOn(nowMs) ? StateShow::Fault : StateShow::Idle;
   } else if(errorLed.isActive()) {
      show = errorLed.isOn() ? StateShow::Fault : StateShow::Idle;
   }
   board::showState(show);
   board::setLed(board::Led::Usb, usbStack.isConfigured());
}

// On the STM32H5, a debugger reads zeros from the memory bus while the
// core sleeps in WFI, and DBGMCU_CR has no DBG_SLEEP bit (#115).
bool isDebuggerConnected() {
#if defined(DCB)
   return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0;
#else
   return (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0;
#endif
}

} // namespace

int main() {
   // os_startup_initialize_hardware() (initialize_hardware.cpp) set up the
   // clocks and the board pins before main().
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
      // The SysTick and the USB interrupt end the WFI.
      // Sleep only when no USB event waits. An event that came after the
      // USB poll has no pending interrupt to end the WFI: the interrupt
      // handler already ran, or the main loop queued the event itself (the
      // double-buffered OUT endpoint reports a waiting packet when it is
      // armed again). With the interrupts masked, an interrupt still ends
      // the WFI, and its handler runs after __enable_irq().
      __disable_irq();
      if(!isDebuggerConnected() && (!clockFromCrystal || !usbStack.hasPendingEvents())) {
         __WFI();
      }
      __enable_irq();
   }
}
