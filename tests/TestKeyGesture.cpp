#include "personality/KeyGesture.h"

#include <doctest/doctest.h>

using personality::KeyGesture;

namespace {

constexpr uint64_t start = 1'000'000;

} // namespace

TEST_CASE("A long press is the gesture at the release") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start + 10));
   // Not while the key is down
   CHECK_FALSE(gesture.update(true, start + 10 + KeyGesture::holdMs));
   CHECK_FALSE(gesture.update(true, start + 10 + 2 * KeyGesture::holdMs));
   CHECK(gesture.update(false, start + 10 + 2 * KeyGesture::holdMs + 1));
   // Once for each press
   CHECK_FALSE(gesture.update(false, start + 10 + 2 * KeyGesture::holdMs + 2));
}

TEST_CASE("The release after exactly holdMs is the gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start + 10));
   CHECK(gesture.update(false, start + 10 + KeyGesture::holdMs));
}

TEST_CASE("A short press is not the gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(false, start));
   CHECK_FALSE(gesture.update(true, start + 10));
   CHECK_FALSE(gesture.update(false, start + 10 + KeyGesture::holdMs - 1));
   // The time starts again at the next press
   CHECK_FALSE(gesture.update(true, start + 10 + KeyGesture::holdMs));
   CHECK_FALSE(gesture.update(false, start + 10 + 2 * KeyGesture::holdMs - 1));
   CHECK_FALSE(gesture.update(true, start + 10 + 2 * KeyGesture::holdMs));
   CHECK(gesture.update(false, start + 10 + 3 * KeyGesture::holdMs));
}

TEST_CASE("A key that is down at the start is not the gesture") {
   KeyGesture gesture;

   CHECK_FALSE(gesture.update(true, start));
   CHECK_FALSE(gesture.update(true, start + 10 * KeyGesture::holdMs));
   // The first release only arms the gesture
   CHECK_FALSE(gesture.update(false, start + 10 * KeyGesture::holdMs + 1));
   CHECK_FALSE(gesture.update(true, start + 10 * KeyGesture::holdMs + 2));
   CHECK(gesture.update(false, start + 11 * KeyGesture::holdMs + 2));
}
