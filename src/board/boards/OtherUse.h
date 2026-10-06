#pragma once

#include "board/boards/Mini.h"

namespace board {

// OtherUse: a Versaloon Mini compatible board with an STM32F103C8 (64 KB
// flash). Only the USB pull-up is on another pin. It reports the hardware
// ID of the Mini, so host tools treat it as a Mini.
inline constexpr Definition otherUse = [] {
   Definition def = mini;
   def.name = "otheruse";
   def.usbPullUp = Signal{ { Port::A, 2 }, Active::High };
   return def;
}();
static_assert(isValid(otherUse));

} // namespace board
