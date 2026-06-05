// SPDX-License-Identifier: Apache-2.0
//
// shimmy — single-host, shared-memory, SPMC messaging library.
//
// Header-only library. Consumers add `include/` to their include path and
// `#include <shimmy/...>`. This version header is the bootstrap stub: it proves
// the header-only library is consumable by both the test and benchmark targets
// end-to-end. The real ring buffer lands in shimmy-4d4.
#ifndef SHIMMY_VERSION_HPP
#define SHIMMY_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace shimmy {

inline constexpr std::uint32_t version_major = 0;
inline constexpr std::uint32_t version_minor = 0;
inline constexpr std::uint32_t version_patch = 0;

// Packed version: (major << 16) | (minor << 8) | patch.
inline constexpr std::uint32_t version_number =
    (version_major << 16) | (version_minor << 8) | version_patch;

inline constexpr std::string_view version_string = "0.0.0";

} // namespace shimmy

#endif // SHIMMY_VERSION_HPP
