// Versaloon next generation firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and on
// each USB interrupt. It runs the USB device and drives the heartbeat on
// the state LED and the USB state on the USB LED.
#include <cmsis_device.h>
#include <utils/Timekeeping.h>

#include "Hardware.h"
#include "Heartbeat.h"
#include "board/Leds.h"
#include "usb/UsbStack.h"

namespace {

usb::UsbStack usbStack;

} // namespace

int main() {
   const bool clockFromCrystal = hardware::isClockFromCrystal();
   const Heartbeat heartbeat{ clockFromCrystal ? Heartbeat::normal : Heartbeat::clockFault };

   // USB needs the 48 MHz clock from the PLL. Without the crystal the
   // device stays disconnected.
   if(clockFromCrystal) {
      usbStack.start();
   }

   for(;;) {
      if(clockFromCrystal) {
         usbStack.poll();
      }
      // One SysTick is 1 ms (initialize_hardware.cpp)
      board::setLed(board::Led::State, heartbeat.isOn(static_cast<uint64_t>(getUptimeTicks())));
      board::setLed(board::Led::Usb, usbStack.isConfigured());
      __WFI();
   }
}
