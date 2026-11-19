// SPDX-License-Identifier: GPL-3.0-or-later
// The exclusion of the JTAG fast path and the SWO Manchester capture on
// DMA2 channel 2 of the CH32V307 (board/SharedDmaChannel.h).

#include "board/SharedDmaChannel.h"

#include <doctest/doctest.h>

namespace {

int stops = 0;

void stopCapture() {
   ++stops;
}

} // namespace

TEST_CASE("Shared DMA channel: JTAG stops a running capture, and a capture needs the channel free") {
   board::SharedDmaChannel channel;
   stops = 0;
   CHECK(channel.takeForCapture(&stopCapture));
   channel.takeForJtag();
   CHECK(stops == 1);
   CHECK(channel.jtagHasIt());
   // While JTAG has the channel, a capture start fails
   CHECK_FALSE(channel.takeForCapture(&stopCapture));
   // A second JTAG setup does not stop the capture again
   channel.takeForJtag();
   CHECK(stops == 1);
   channel.releaseFromJtag();
   CHECK(channel.takeForCapture(&stopCapture));
   // A capture that stopped itself is not stopped by the next JTAG setup
   channel.releaseFromCapture();
   channel.takeForJtag();
   CHECK(stops == 1);
}
