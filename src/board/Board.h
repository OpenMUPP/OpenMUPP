#pragma once

// The board of this build: board::current. CMake defines one of
// VSL_BOARD_MINI, VSL_BOARD_OTHERUSE or VSL_BOARD_STLINKV2 from BOARD.
// The protocol and the engines read board data only through board::current.
// With VSL_DEBUG_SWD, board::current is the debug variant of the board.

#if defined(VSL_BOARD_MINI)
#include "board/boards/Mini.h"
#elif defined(VSL_BOARD_OTHERUSE)
#include "board/boards/OtherUse.h"
#elif defined(VSL_BOARD_STLINKV2)
#include "board/boards/StLinkV2.h"
#else
#error "No board selected: define VSL_BOARD_MINI, VSL_BOARD_OTHERUSE or VSL_BOARD_STLINKV2"
#endif

namespace board {

#if defined(VSL_BOARD_MINI)
inline constexpr const Definition &selected = mini;
#elif defined(VSL_BOARD_OTHERUSE)
inline constexpr const Definition &selected = otherUse;
#elif defined(VSL_BOARD_STLINKV2)
inline constexpr const Definition &selected = stLinkV2;
#endif

// A debug build with VSL_DEBUG_SWD keeps the SW-DP of the dongle MCU on
// (plan section 3). It drops the LEDs on the SWD pins (see withSwd()).
#if defined(VSL_DEBUG_SWD)
inline constexpr Definition debugSelected = withSwd(selected);
static_assert(swjConfig(debugSelected) != SwjConfig::Disabled, "VSL_DEBUG_SWD: the board uses an SWD pin for more than an LED");
inline constexpr const Definition &current = debugSelected;
#else
inline constexpr const Definition &current = selected;
#endif

} // namespace board
