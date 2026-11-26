// SPDX-License-Identifier: GPL-3.0-or-later

#include "boot/BootloaderEntry.h"
#include "personality/PersonalitySwitch.h"

#include <doctest/doctest.h>

using boot::BootloaderEntry;

TEST_CASE("The boot request is the value of openmupp-bootloader") {
   CHECK(boot::bootRequest == 0xB007);
}

TEST_CASE("The bootloader entry is not due without a request") {
   BootloaderEntry entry;

   CHECK_FALSE(entry.isRequested());
   CHECK_FALSE(entry.isDue(0));
   CHECK_FALSE(entry.isDue(1'000'000));
}

TEST_CASE("The bootloader entry is due after the reply delay") {
   BootloaderEntry entry;
   constexpr uint64_t start = 5'000'000'000'000;

   entry.request();
   CHECK(entry.isRequested());
   CHECK_FALSE(entry.isDue(start));
   CHECK_FALSE(entry.isDue(start + BootloaderEntry::replyDelayMs - 1));
   CHECK(entry.isDue(start + BootloaderEntry::replyDelayMs));
   CHECK(entry.isDue(start + BootloaderEntry::replyDelayMs + 1));
}

TEST_CASE("A second request does not move the bootloader entry") {
   BootloaderEntry entry;

   entry.request();
   CHECK_FALSE(entry.isDue(100));
   entry.request();
   CHECK_FALSE(entry.isDue(105));
   CHECK(entry.isDue(100 + BootloaderEntry::replyDelayMs));
}

TEST_CASE("A delayed request waits until the key is up for the debounce time") {
   boot::DelayedRequest reset;
   reset.request();

   CHECK_FALSE(reset.isDue(100, true));
   CHECK_FALSE(reset.isDue(200, true));
   // A bounce: up, down again, then up for good at 300
   CHECK_FALSE(reset.isDue(250, false));
   CHECK_FALSE(reset.isDue(252, true));
   CHECK_FALSE(reset.isDue(300, false));
   CHECK_FALSE(reset.isDue(300 + boot::DelayedRequest::releaseDebounceMs - 1, false));
   CHECK(reset.isDue(300 + boot::DelayedRequest::releaseDebounceMs, false));
}

TEST_CASE("A delayed request with the key up the whole time has no debounce wait") {
   boot::DelayedRequest reset;
   reset.request();

   CHECK_FALSE(reset.isDue(100, false));
   CHECK(reset.isDue(100 + boot::DelayedRequest::replyDelayMs, false));
}

TEST_CASE("The debounce time is the one of the personality switch") {
   CHECK(boot::DelayedRequest::releaseDebounceMs == personality::PersonalitySwitch::releaseDebounceMs);
}
