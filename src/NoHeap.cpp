// SPDX-License-Identifier: GPL-3.0-or-later
// The firmware has no heap. These definitions replace the newlib and
// libstdc++ versions, which link malloc, free and the newlib locks (#75).
//
// - The deleting destructor of a class with a virtual destructor refers to
//   operator delete. The firmware never calls it: all objects are static.
// - assert.c of system-stm32 calls abort(). The newlib abort() links raise
//   and signal.
//
// Each one is a trap: the HardFault handler of system-stm32 then reports
// it.

#include <cstddef>
#include <cstdlib>

void operator delete(void *) noexcept {
   __builtin_trap();
}

void operator delete(void *, std::size_t) noexcept {
   __builtin_trap();
}

extern "C" [[noreturn]] void abort() noexcept {
   __builtin_trap();
}
