# Correctness + concurrency (TSan) tests. Compiled target linking GoogleTest.
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(shimmy_tests
  smoke_test.cpp)

target_link_libraries(shimmy_tests PRIVATE
  shimmy::shimmy
  shimmy_warnings
  GTest::gtest
  GTest::gtest_main)

# Optional ThreadSanitizer build. Concurrency stress tests (lap detection,
# overflow invariants) in shimmy-4d4 will rely on this.
if(SHIMMY_ENABLE_TSAN)
  target_compile_options(shimmy_tests PRIVATE -fsanitize=thread -g -O1)
  target_link_options(shimmy_tests PRIVATE -fsanitize=thread)
endif()

gtest_discover_tests(shimmy_tests)
