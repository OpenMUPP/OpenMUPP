#pragma once

#include <cstdint>

namespace protocol {

// Waits: the DELAY block of USB_TO_ALL and the settle time of the target
// power (TargetPower.h).
class Delay {
   public:
   virtual void microseconds(uint16_t us) = 0;
   virtual void milliseconds(uint16_t ms) = 0;

   protected:
   ~Delay() = default;
};

} // namespace protocol
