// Versaloon next generation firmware. os_startup_initialize_hardware()
// (src/initialize_hardware.cpp) sets up the clocks and the board pins
// before main() runs. The main loop wakes up on each 1 ms SysTick and
// drives the heartbeat on the state LED.
#include <cmsis_device.h>
#include <utils/Timekeeping.h>

#include "Hardware.h"
#include "Heartbeat.h"
#include "board/Leds.h"

int main() {
   const Heartbeat heartbeat{ hardware::isClockFromCrystal() ? Heartbeat::normal : Heartbeat::clockFault };

   for(;;) {
      // One SysTick is 1 ms (initialize_hardware.cpp)
      board::setLed(board::Led::State, heartbeat.isOn(static_cast<uint64_t>(getUptimeTicks())));
      __WFI();
   }
}
