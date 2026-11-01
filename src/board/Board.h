#pragma once

// The board of this build: board::current. CMake defines one of
// MUPP_BOARD_MINI, MUPP_BOARD_OTHERUSE, MUPP_BOARD_STLINKV2,
// MUPP_BOARD_STLINKV2MINICLONE or MUPP_BOARD_WEACTH523 from BOARD.
// The protocol and the engines read board data only through board::current.
// With MUPP_DEBUG_SWD, board::current is the debug variant of the board.

#if defined(MUPP_BOARD_MINI)
#include "board/boards/Mini.h"
#elif defined(MUPP_BOARD_OTHERUSE)
#include "board/boards/OtherUse.h"
#elif defined(MUPP_BOARD_STLINKV2)
#include "board/boards/StLinkV2.h"
#elif defined(MUPP_BOARD_STLINKV2MINICLONE)
#include "board/boards/StLinkV2MiniClone.h"
#elif defined(MUPP_BOARD_WEACTH523)
#include "board/boards/WeActH523.h"
#else
#error "No board selected: define MUPP_BOARD_MINI, MUPP_BOARD_OTHERUSE, MUPP_BOARD_STLINKV2, MUPP_BOARD_STLINKV2MINICLONE or MUPP_BOARD_WEACTH523"
#endif

namespace board {

#if defined(MUPP_BOARD_MINI)
inline constexpr const Definition &selected = mini;
#elif defined(MUPP_BOARD_OTHERUSE)
inline constexpr const Definition &selected = otherUse;
#elif defined(MUPP_BOARD_STLINKV2)
inline constexpr const Definition &selected = stLinkV2;
#elif defined(MUPP_BOARD_STLINKV2MINICLONE)
inline constexpr const Definition &selected = stLinkV2MiniClone;
#elif defined(MUPP_BOARD_WEACTH523)
inline constexpr const Definition &selected = weActH523;
#endif

// A debug build with MUPP_DEBUG_SWD keeps the SW-DP of the dongle MCU on
// (plan section 3). It drops the LEDs on the SWD pins (see withSwd()).
#if defined(MUPP_DEBUG_SWD)
inline constexpr Definition debugSelected = withSwd(selected);
static_assert(swjConfig(debugSelected) != SwjConfig::Disabled, "MUPP_DEBUG_SWD: the board uses an SWD pin for more than an LED");
inline constexpr const Definition &current = debugSelected;
#else
inline constexpr const Definition &current = selected;
#endif

} // namespace board
