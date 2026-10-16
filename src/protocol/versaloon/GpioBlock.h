#pragma once

// The USB_TO_GPIO block (0x23): OpenOCD drives SRST and TRST through it.
// Hardware-independent: the block maps the requests to a GpioPins object,
// and the host unit tests use a fake one.
//
// Sub-commands (port index 0 only, all values u16 little-endian):
//
//   INIT    []                              -> [status]
//   FINI    []                              -> [status]
//   CONFIG  [mask][io][pull_en][pull/out]   -> [status]
//   OUT     [mask][value]                   -> [status]
//   IN      [mask]                          -> [status][value u16]
//
// The bits of mask (gpio::srst, ...) select the pins. CONFIG sets each
// selected pin:
//
//   io  pull_en  pull/out  pin
//   1   0        level     push-pull output at the level
//   1   1        level     open-drain output at the level
//   0   0        -         floating input
//   0   1        1         input with pull-up
//   0   1        0         input with pull-down
//
// OUT writes the output level of the selected pins. On the STM32F1, the
// output register of an input with pull selects the pull, as in the
// original firmware. IN reads the level of the selected pins.
//
// A pin that the board does not have is ignored, and reads as 0. A bit
// above gpio::tms, or an output on TDO or RTCK, is INVALID_PARA.
//
// hostGone() releases SRST and TRST: inputs with pull-up, as OpenOCD
// releases SRST. Thus a target does not stay in reset when OpenOCD stops
// while it asserts SRST.

#include <cstdint>

#include "protocol/GpioPins.h"
#include "protocol/versaloon/UsbToXxx.h"

namespace protocol {

class GpioBlock final : public SubCommandBlock {
   public:
   explicit GpioBlock(GpioPins &pins);

   void hostGone() override;

   protected:
   void runSubCommand(const SubCommand &command, Reply &reply) override;

   private:
   [[nodiscard]] Status config(art::span<const uint8_t> payload);
   [[nodiscard]] Status out(art::span<const uint8_t> payload);

   GpioPins &pins_;
};

} // namespace protocol
