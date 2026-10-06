// Versaloon next generation firmware. This is the empty project skeleton:
// the system-stm32 start-up code sets VTOR and runs main(), which idles.
// The board bring-up (clocks, LEDs, logger) follows in a later change.
#include <cmsis_device.h>

int main() {
   for(;;) {
      __WFI();
   }
}
