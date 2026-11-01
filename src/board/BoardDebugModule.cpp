// SPDX-License-Identifier: GPL-3.0-or-later

#include "board/BoardRvSwd.h"

// The debug module layer on the RISC-V debug wire engine, with the -Os of
// the other sources: it is not timing-critical.
template class rvswd::DebugModule<board::BoardRvSwd>;
