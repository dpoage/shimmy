// SPDX-License-Identifier: Apache-2.0
//
// shimmy — cache-line constants and false-sharing helpers.
#ifndef SHIMMY_DETAIL_CACHE_HPP
#define SHIMMY_DETAIL_CACHE_HPP

#include <cstddef>
#include <new>

namespace shimmy::detail {

// std::hardware_destructive_interference_size is the "correct" value, but it is
// notoriously inconsistent across stdlibs: libstdc++ warns about ABI stability
// when it is used in a struct layout (-Winterference-size), and libc++ did not
// define it at all for a long time. Because this value participates in the
// on-disk / in-shmem layout of the ring (shimmy-52e), it MUST be a fixed
// compile-time constant that does not vary between compilers/stdlib versions —
// otherwise a producer built with gcc and a consumer built with clang could
// disagree on the byte layout of a shared segment. So we hard-code 64 (the
// x86-64 / Apple-silicon cache line) rather than depend on the library value.
// 64 is correct for every x86-64 microarch shimmy targets in phase 1.
inline constexpr std::size_t cache_line_size = 64;

} // namespace shimmy::detail

#endif // SHIMMY_DETAIL_CACHE_HPP
