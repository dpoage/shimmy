# Correctness + concurrency (TSan) tests. Compiled target linking GoogleTest.
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

add_executable(shimmy_tests
  smoke_test.cpp
  ring_basic_test.cpp
  ring_lap_test.cpp
  ring_concurrency_test.cpp
  shm_segment_test.cpp)

target_link_libraries(shimmy_tests PRIVATE
  shimmy::shimmy
  shimmy_warnings
  GTest::gtest
  GTest::gtest_main
  Threads::Threads)

# shm_open/shm_unlink live in librt on some libc versions; link it on platforms
# that provide it (the shared-memory segment layer, shimmy-52e, needs it).
if(UNIX AND NOT APPLE)
  target_link_libraries(shimmy_tests PRIVATE rt)
endif()

# Optional ThreadSanitizer build. Concurrency stress tests (lap detection,
# overflow invariants) in shimmy-4d4 will rely on this.
#
# shimmy-52e: the cross-process fork() round-trip test is INCOMPATIBLE with
# ThreadSanitizer. TSan's runtime is not designed for our fork-then-consume-in-
# child pattern over shared memory (it intercepts fork and re-initializes
# runtime state, producing spurious reports/hangs), and the producer and
# consumer run in genuinely separate address spaces where TSan cannot see the
# cross-process happens-before through shared memory anyway. We compile that ONE
# test out under TSan via SHIMMY_TSAN_NO_FORK; every other shm test (header
# validation, teardown, hugepage fallback, in-process Backpressure attach) still
# runs clean under TSan. The shared Ring protocol's data-race freedom is already
# covered by the in-process TSan stress in shimmy-4d4 (identical Ring bytes and
# atomics — only the addressing differs cross-process).
if(SHIMMY_ENABLE_TSAN)
  target_compile_options(shimmy_tests PRIVATE -fsanitize=thread -g -O1)
  target_link_options(shimmy_tests PRIVATE -fsanitize=thread)
  target_compile_definitions(shimmy_tests PRIVATE SHIMMY_TSAN_NO_FORK=1)
endif()

# Optional LLVM source-based coverage (clang only). Mirrors the TSan option:
# instrument the compiled test target, then llvm-profdata merge + llvm-cov report
# over include/shimmy/* yields the measured library coverage. The library is
# header-only, so coverage of the headers is realized entirely through the test
# translation units that include them.
if(SHIMMY_ENABLE_COVERAGE)
  target_compile_options(shimmy_tests PRIVATE
    -fprofile-instr-generate -fcoverage-mapping -g -O0)
  target_link_options(shimmy_tests PRIVATE -fprofile-instr-generate)
endif()

gtest_discover_tests(shimmy_tests)
