#include "boot/BootloaderEntry.h"

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
