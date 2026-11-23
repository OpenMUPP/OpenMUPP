// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardSwd.h"
#include "board/BoardTargetDrive.h"

// The SWD engine, compiled with -O2 (CMakeLists.txt).
template class swd::BitBangPort<board::SwdPins>;

namespace board {

namespace {

using hw::GpioPin;

// The output register first, so the pin starts at its level.
void configureOutput(Pin pin, bool level) {
   const GpioPin gpio = gpioPin(pin);
   gpio.write(level);
   gpio.configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::None, GpioPin::Speed::VeryHigh);
}

} // namespace

void SwdPins::init() {
   const DebugPins &pins = current.debug;
   // An open SWDIO reads 1: no target gives the ACK 0b111.
   if(pins.tmsIn != pins.tmsOut) {
      gpioPin(pins.tmsIn).configure(GpioPin::Mode::Input, GpioPin::OutputType::PushPull, GpioPin::Pull::Up);
   }
   // SWDIO has its own shifter direction (DIR_SWDIO): only SWCLK is in the
   // shifter group of driveTargetOutputs(). DIR_SWDIO goes high after the
   // SWDIO output, as in swdioDrive(). fini() sets it low first.
   driveTargetOutputs(true, [&] { configureOutput(pins.tck, true); });
   configureOutput(pins.tmsOut, true);
   SwdioDir::set();
#if !defined(STM32F1) && !defined(__riscv)
   // The pull-up of swdioRelease() (board/BoardSwd.h). A pull-up on a
   // push-pull output has no effect. The F1-style GPIO (STM32F1, CH32) has no pull on outputs
   // (GpioPin::configure() asserts it): swdioRelease() sets the pull-up input instead.
   gpioPin(pins.tmsOut).configure(GpioPin::Mode::Output, GpioPin::OutputType::PushPull, GpioPin::Pull::Up, GpioPin::Speed::VeryHigh);
#endif
#if defined(__riscv)
   // The SWDIO direction words, so they are never 0 (all PC8 to PC15
   // analog inputs). The engines call begin() again before each operation.
   begin();
#endif
}

void SwdPins::fini() {
   const DebugPins &pins = current.debug;
   driveTargetOutputs(false, [&] {
      for(const Pin pin : { pins.tck, pins.tmsOut }) {
         gpioPin(pin).configure(GpioPin::Mode::Input);
      }
   });
}

#if defined(__riscv)
// The NOP sled of SwdPins::padSled(): padSledNops compressed NOPs (one
// cycle each), then the return. A jump to an entry before the return runs
// that many NOPs. Compressed instructions on their own, so each NOP is 2
// bytes.
static_assert(SwdPins::padSledNops == 32, "The .rept count below");

extern "C" const uint8_t swdPadSledEnd[];
asm(".pushsection .text.swdPadSled,\"ax\",@progbits\n"
    ".balign 4\n"
    ".option push\n"
    ".option rvc\n"
    ".rept 32\n"
    "c.nop\n"
    ".endr\n"
    ".globl swdPadSledEnd\n"
    "swdPadSledEnd:\n"
    "c.jr ra\n"
    ".option pop\n"
    ".popsection\n");

// nopCount is at most padSledNops (static_assert in BoardSwd.h).
void SwdPins::setPadSled(uint32_t nopCount) {
   padSledEntry_ = reinterpret_cast<uintptr_t>(swdPadSledEnd) - 2 * nopCount;
}
#endif

} // namespace board
