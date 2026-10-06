#pragma once

// The board of this build: board::current. CMake defines one of
// VSL_BOARD_MINI, VSL_BOARD_OTHERUSE or VSL_BOARD_STLINKV2 from BOARD.
// The protocol and the engines read board data only through board::current.

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
inline constexpr const Definition &current = mini;
#elif defined(VSL_BOARD_OTHERUSE)
inline constexpr const Definition &current = otherUse;
#elif defined(VSL_BOARD_STLINKV2)
inline constexpr const Definition &current = stLinkV2;
#endif

} // namespace board
