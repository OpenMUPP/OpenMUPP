// SPDX-License-Identifier: GPL-3.0-or-later

#include "personality/KeyGesture.h"

#include <doctest/doctest.h>

using personality::KeyGesture;

namespace {

constexpr uint64_t start = 1'000'000;

} // namespace

TEST_CASE("A long press is the gesture when holdMs have passed") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start + 10));
   CHECK_FALSE(gesture.update(true, start + 10 + KeyGesture::holdMs - 1));
   CHECK(gesture.update(true, start + 10 + KeyGesture::holdMs));
   // Once for each press, and not again at the release
   CHECK_FALSE(gesture.update(true, start + 10 + 2 * KeyGesture::holdMs));
   CHECK_FALSE(gesture.update(false, start + 10 + 2 * KeyGesture::holdMs + 1));
   CHECK_FALSE(gesture.update(false, start + 10 + 2 * KeyGesture::holdMs + 2));
}

TEST_CASE("A short press is not the gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start + 10));
   CHECK_FALSE(gesture.update(true, start + 10 + KeyGesture::holdMs - 1));
   CHECK_FALSE(gesture.update(false, start + 10 + KeyGesture::holdMs - 1));
   // The time starts again at the next press
   CHECK_FALSE(gesture.update(true, start + 10 + KeyGesture::holdMs));
   CHECK_FALSE(gesture.update(true, start + 10 + 2 * KeyGesture::holdMs - 1));
   CHECK(gesture.update(true, start + 10 + 2 * KeyGesture::holdMs));
}

TEST_CASE("Each press gives one gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start));
   CHECK(gesture.update(true, start + KeyGesture::holdMs));
   CHECK_FALSE(gesture.update(false, start + KeyGesture::holdMs + 1));
   CHECK_FALSE(gesture.update(true, start + KeyGesture::holdMs + 2));
   CHECK(gesture.update(true, start + 2 * KeyGesture::holdMs + 2));
}

TEST_CASE("A key that is down at the start is not the gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(true, start));
   CHECK_FALSE(gesture.update(true, start + 10 * KeyGesture::holdMs));
   // The first release only arms the gesture
   CHECK_FALSE(gesture.update(false, start + 10 * KeyGesture::holdMs + 1));
   CHECK_FALSE(gesture.update(true, start + 10 * KeyGesture::holdMs + 2));
   CHECK(gesture.update(true, start + 11 * KeyGesture::holdMs + 2));
}
