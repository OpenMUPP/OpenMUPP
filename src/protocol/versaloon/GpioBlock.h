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

#include "protocol/versaloon/UsbToXxx.h"

namespace protocol {

// The bits of the GPIO block
namespace gpio {
inline constexpr uint16_t srst = 0x0001;
inline constexpr uint16_t trst = 0x0002;
inline constexpr uint16_t usr1 = 0x0004;
inline constexpr uint16_t usr2 = 0x0008;
inline constexpr uint16_t tck = 0x0010;
inline constexpr uint16_t tdo = 0x0020;
inline constexpr uint16_t tdi = 0x0040;
inline constexpr uint16_t rtck = 0x0080;
inline constexpr uint16_t tms = 0x0100;

inline constexpr uint16_t all = 0x01FF;
// The pins that can only be inputs
inline constexpr uint16_t inputOnly = tdo | rtck;
} // namespace gpio

enum class PinMode : uint8_t {
   Input,
   InputPullUp,
   InputPullDown,
   PushPull,
   OpenDrain,
};

// The debug connector pins, addressed by one gpio bit each.
class GpioPins {
   public:
   // The gpio bits of the pins that the board has
   [[nodiscard]] virtual uint16_t available() const = 0;
   // Sets the mode of one pin (bit: one gpio bit of available()). For an
   // output, level is the output level; the pin does not glitch to the
   // other level.
   virtual void configure(uint16_t bit, PinMode mode, bool level) = 0;
   // Writes the output register of the pins of mask (bits of available()).
   virtual void write(uint16_t mask, uint16_t value) = 0;
   // Reads the level of the pins of mask (bits of available()).
   [[nodiscard]] virtual uint16_t read(uint16_t mask) const = 0;

   protected:
   ~GpioPins() = default;
};

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
