// SPDX-License-Identifier: Apache-2.0
//
// shimmy — overflow-policy tag types.
//
// The overflow policy is a *compile-time* axis (DESIGN §3). It is expressed as
// an empty tag type so that the producer's hot path collapses to straight-line
// code: there is no runtime branch on "which policy", only `if constexpr`
// dispatch that the optimizer removes entirely.
#ifndef SHIMMY_POLICIES_HPP
#define SHIMMY_POLICIES_HPP

#include <type_traits>

namespace shimmy {

// Overwrite: the producer NEVER blocks. It free-runs and may lap slow
// consumers. A lapped consumer detects the resulting sequence gap, reports the
// loss, and resyncs. Throughput-optimal, bounded producer latency, lossy under
// pressure. (DESIGN §3 — overflow invariant 1.)
struct Overwrite {};

// Backpressure: the producer is LOSSLESS. Before it may reuse a slot it waits
// until every consumer's cursor has advanced past the sequence currently
// occupying that slot. One slow consumer stalls the producer — producer latency
// is coupled to the slowest consumer. (DESIGN §3 — overflow invariant 2.)
struct Backpressure {};

template <typename T>
inline constexpr bool is_overflow_policy_v =
    std::is_same_v<T, Overwrite> || std::is_same_v<T, Backpressure>;

} // namespace shimmy

#endif // SHIMMY_POLICIES_HPP
