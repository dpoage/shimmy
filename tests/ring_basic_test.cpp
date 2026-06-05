// SPDX-License-Identifier: Apache-2.0
//
// Single-threaded correctness for the core ring: layout invariants, basic
// publish/read, zero-copy view shape, read_copy, wait-strategy instantiation.
#include <shimmy/ring.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

using shimmy::Backpressure;
using shimmy::Consumer;
using shimmy::Overwrite;
using shimmy::read_status;
using shimmy::Ring;

// The ring must be standard-layout and free of internal pointers so it can later
// live in a shared-memory segment (shimmy-52e). We assert the structural
// properties that make that possible.
TEST(Layout, RingIsShmemPlaceable) {
  using R = Ring<64, 8, Overwrite>;
  // No vtables: trivially-default-constructible after init(), standard layout.
  static_assert(std::is_standard_layout_v<R>,
                "Ring must be standard-layout for shmem placement");
  static_assert(!std::is_polymorphic_v<R>, "Ring must have no vtable");
  // The slot is standard-layout too.
  static_assert(std::is_standard_layout_v<typename R::slot_type>);
  SUCCEED();
}

TEST(Layout, CapacityMustBePowerOfTwo) {
  // These would static_assert if instantiated; we just confirm valid ones work.
  static_assert(Ring<64, 2, Overwrite>::capacity == 2);
  static_assert(Ring<64, 1024, Overwrite>::capacity == 1024);
  static_assert(Ring<64, 1024, Overwrite>::mask == 1023);
  SUCCEED();
}

TEST(Basic, PublishThenReadInOrder) {
  Ring<64, 8, Overwrite> ring;
  Consumer c(ring);

  for (std::uint32_t i = 0; i < 5; ++i) {
    const std::uint32_t payload = i * 7 + 1;
    ring.publish(&payload, sizeof(payload));
  }

  for (std::uint32_t i = 0; i < 5; ++i) {
    auto m = c.read();
    ASSERT_EQ(c.status(), read_status::ok) << "at i=" << i;
    ASSERT_EQ(m.size(), sizeof(std::uint32_t));
    std::uint32_t got = 0;
    std::memcpy(&got, m.data(), m.size());
    EXPECT_EQ(got, i * 7 + 1);
    EXPECT_TRUE(c.validate());
    c.commit();
  }
  // Nothing left.
  (void)c.read();
  EXPECT_EQ(c.status(), read_status::empty);
}

TEST(Basic, EmptyRingReadsEmpty) {
  Ring<64, 4, Overwrite> ring;
  Consumer c(ring);
  (void)c.read();
  EXPECT_EQ(c.status(), read_status::empty);
}

TEST(Basic, ReadCopyMatchesZeroCopy) {
  Ring<128, 8, Overwrite> ring;
  Consumer c(ring);
  const std::string msg = "hello shimmy";
  ring.publish(msg.data(), static_cast<std::uint32_t>(msg.size()));

  std::array<char, 128> buf{};
  const std::size_t n = c.read_copy(buf.data(), buf.size());
  ASSERT_EQ(c.status(), read_status::ok);
  ASSERT_EQ(n, msg.size());
  EXPECT_EQ(std::string(buf.data(), n), msg);
}

TEST(Basic, VariableLengthPayloadsWithinBlock) {
  Ring<256, 8, Overwrite> ring;
  Consumer c(ring);
  const std::string a(10, 'a');
  const std::string b(200, 'b');
  ring.publish(a.data(), static_cast<std::uint32_t>(a.size()));
  ring.publish(b.data(), static_cast<std::uint32_t>(b.size()));

  auto m1 = c.read();
  ASSERT_EQ(c.status(), read_status::ok);
  EXPECT_EQ(m1.size(), 10u);
  c.commit();
  auto m2 = c.read();
  ASSERT_EQ(c.status(), read_status::ok);
  EXPECT_EQ(m2.size(), 200u);
  c.commit();
}

// All three wait strategies must instantiate AND pass a basic read via the
// blocking path (acceptance criterion 7). Producer publishes first so the
// blocking read returns immediately (single-threaded smoke).
template <typename Wait>
void wait_strategy_smoke() {
  Ring<64, 8, Overwrite, 16, Wait> ring;
  Consumer c(ring);
  const std::uint64_t v = 0xABCDEF;
  ring.publish(&v, sizeof(v));
  auto m = c.read_blocking();
  ASSERT_EQ(c.status(), read_status::ok);
  std::uint64_t got = 0;
  std::memcpy(&got, m.data(), m.size());
  EXPECT_EQ(got, v);
  c.commit();
}

TEST(WaitStrategy, SpinInstantiatesAndReads) {
  wait_strategy_smoke<shimmy::Spin>();
}
TEST(WaitStrategy, SpinThenYieldInstantiatesAndReads) {
  wait_strategy_smoke<shimmy::SpinThenYield<256>>();
}
TEST(WaitStrategy, FutexInstantiatesAndReads) {
  wait_strategy_smoke<shimmy::Futex>();
}

} // namespace
