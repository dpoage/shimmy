// Bootstrap smoke test: proves the test toolchain (C++20 + GoogleTest) links
// against the header-only library and runs. Real correctness tests (ring
// sequencing, overflow-policy invariants, lap detection, TSan stress) land in
// shimmy-4d4.
#include <shimmy/version.hpp>

#include <gtest/gtest.h>

TEST(Bootstrap, VersionHeaderIsConsumable) {
  EXPECT_EQ(shimmy::version_major, 0u);
  EXPECT_EQ(shimmy::version_string, "0.0.0");
  EXPECT_EQ(shimmy::version_number, 0u);
}

TEST(Bootstrap, Cxx20IsAvailable) {
  // constexpr std::string_view comparison is a C++20-friendly sanity check.
  static_assert(shimmy::version_string == "0.0.0");
  SUCCEED();
}
