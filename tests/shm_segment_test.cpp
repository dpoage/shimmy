// SPDX-License-Identifier: Apache-2.0
//
// shimmy — shared-memory segment lifecycle tests (bead shimmy-52e).
//
// Covers the four acceptance criteria:
//   1. Cross-process round trip via fork(): creator/producer in one process,
//      opener/consumer in a genuinely separate address space, payloads verified.
//   2. Versioned header: opening with an incompatible layout (wrong magic /
//      corrupted version / mismatched Ring instantiation) fails LOUDLY with a
//      shm_layout_error, never silent corruption.
//   3. Hugepage path: works when available, cleanly falls back / is skipped when
//      not — never fails CI on a host without reserved hugepages.
//   4. Clean teardown: after a normal creator run the name is unlinked, so a
//      second shm_open without O_CREAT fails ENOENT. No leaked segments.
#include <shimmy/shm_segment.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using shimmy::Backpressure;
using shimmy::Consumer;
using shimmy::Overwrite;
using shimmy::policy_id;
using shimmy::read_status;
using shimmy::Ring;
using shimmy::SegmentHeader;
using shimmy::ShmSegment;
using shimmy::shm_layout_error;
using shimmy::shm_os_error;

// A unique-ish name per test so parallel ctest shards / reruns don't collide.
std::string unique_name(const char* tag) {
  return std::string("/shimmy_t52e_") + tag + "_" + std::to_string(::getpid());
}

// Belt-and-suspenders: make sure no stale segment of this name survives a
// previous crashed run before we create.
void pre_unlink(const std::string& name) { ::shm_unlink(name.c_str()); }

// ---------------------------------------------------------------------------
// 1. Cross-process round trip (fork): parent creates + publishes, child opens
//    in a SEPARATE address space and reads every message back via memcpy.
// ---------------------------------------------------------------------------
#if !defined(SHIMMY_TSAN_NO_FORK)
TEST(ShmSegment, CrossProcessRoundTripFork) {
  using R = Ring<64, 1024, Overwrite>;
  const std::string name = unique_name("rt");
  pre_unlink(name);

  constexpr std::uint32_t N = 500;

  // Creator maps the segment BEFORE fork, so the child can open it immediately.
  auto seg = ShmSegment<R>::create(name);
  ASSERT_TRUE(seg.valid());
  ASSERT_TRUE(seg.is_creator());

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0) << "fork failed";

  if (pid == 0) {
    // ---- CHILD: opener / consumer in its own address space. ----
    // Note the child inherited `seg`'s mapping via fork, but we deliberately
    // OPEN the named segment afresh to exercise the cross-process open path and
    // a (potentially) different virtual address.
    int rc = 0;
    try {
      auto view = ShmSegment<R>::open(name);
      Consumer<R> c(*view);

      std::uint32_t got = 0;
      while (got < N) {
        auto m = c.read();
        if (c.status() == read_status::ok) {
          if (m.size() != sizeof(std::uint32_t)) {
            rc = 10;
            break;
          }
          std::uint32_t val = 0;
          std::memcpy(&val, m.data(), m.size());
          if (!c.validate()) {
            continue; // lapped mid-read; retry (shouldn't happen, Cap>N)
          }
          if (val != got * 3u + 7u) {
            rc = 20;
            break;
          }
          c.commit();
          ++got;
        } else if (c.status() == read_status::lapped) {
          rc = 30; // Cap (1024) > N (500): must never lap. Fail if it does.
          break;
        }
        // empty: spin until the producer catches up.
      }
      if (rc == 0 && got != N) {
        rc = 40;
      }
    } catch (const std::exception&) {
      rc = 99;
    }
    _exit(rc);
  }

  // ---- PARENT: producer. Publish N messages the child will read. ----
  for (std::uint32_t i = 0; i < N; ++i) {
    const std::uint32_t payload = i * 3u + 7u;
    seg->publish(&payload, sizeof(payload));
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status)) << "child crashed";
  EXPECT_EQ(WEXITSTATUS(status), 0)
      << "child consumer reported failure code " << WEXITSTATUS(status);

  // Creator teardown unlinks the name.
}
#endif // !SHIMMY_TSAN_NO_FORK

// ---------------------------------------------------------------------------
// 2a. Versioned header — wrong magic / corrupted header fails loudly.
// ---------------------------------------------------------------------------
TEST(ShmSegment, OpenRejectsCorruptedMagic) {
  using R = Ring<64, 16, Overwrite>;
  const std::string name = unique_name("badmagic");
  pre_unlink(name);

  {
    auto seg = ShmSegment<R>::create(name);
    ASSERT_TRUE(seg.valid());

    // Corrupt the magic in-place through a second mapping of the same named
    // segment, simulating a foreign or damaged segment. The opener must reject
    // it, not reinterpret it. The header sits at offset 0 of the mapping.
    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    ASSERT_GE(fd, 0);
    void* p = ::mmap(nullptr, sizeof(SegmentHeader), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ASSERT_NE(p, MAP_FAILED);
    auto* h = static_cast<SegmentHeader*>(p);
    h->magic ^= 0xDEADBEEFULL; // wreck the magic
    ::munmap(p, sizeof(SegmentHeader));
    ::close(fd);

    EXPECT_THROW({ auto bad = ShmSegment<R>::open(name); }, shm_layout_error);
  }
  // seg destructed -> unlinked.
}

// ---------------------------------------------------------------------------
// 2b. Versioned header — opening with a DIFFERENT Ring instantiation (mismatched
//     BlockSize / Capacity / policy) fails loudly. This is the realistic case:
//     two builds disagree on the compile-time channel shape.
// ---------------------------------------------------------------------------
TEST(ShmSegment, OpenRejectsMismatchedInstantiation) {
  using Creator = Ring<64, 16, Overwrite>;
  const std::string name = unique_name("mismatch");
  pre_unlink(name);

  // Type aliases first: a templated type with commas cannot be written inline
  // inside a macro argument (the preprocessor splits on the commas).
  using WrongCapacity = Ring<64, 32, Overwrite>;
  using WrongBlock = Ring<128, 16, Overwrite>;
  using WrongPolicy = Ring<64, 16, Backpressure>;
  using WrongConsumers = Ring<64, 16, Overwrite, 8>;

  auto seg = ShmSegment<Creator>::create(name);
  ASSERT_TRUE(seg.valid());

  EXPECT_THROW({ auto v = ShmSegment<WrongCapacity>::open(name); },
               shm_layout_error);
  EXPECT_THROW({ auto v = ShmSegment<WrongBlock>::open(name); },
               shm_layout_error);
  EXPECT_THROW({ auto v = ShmSegment<WrongPolicy>::open(name); },
               shm_layout_error);
  EXPECT_THROW({ auto v = ShmSegment<WrongConsumers>::open(name); },
               shm_layout_error);

  // The MATCHING instantiation opens fine — proves we reject mismatches, not
  // everything.
  EXPECT_NO_THROW({
    auto v = ShmSegment<Creator>::open(name);
    EXPECT_TRUE(v.valid());
    EXPECT_FALSE(v.is_creator());
  });
}

// ---------------------------------------------------------------------------
// 3. Hugepage path: request hugepages; if available the mapping reports active,
//    if not it falls back cleanly. NEVER fails on a host without hugepages.
// ---------------------------------------------------------------------------
TEST(ShmSegment, HugepagePathWorksOrFallsBack) {
  using R = Ring<64, 64, Overwrite>;
  const std::string name = unique_name("huge");
  pre_unlink(name);

  shimmy::segment_options opts;
  opts.hugepages = true;

  auto seg = ShmSegment<R>::create(name, opts);
  ASSERT_TRUE(seg.valid());

  if (seg.hugepages_active()) {
    GTEST_LOG_(INFO) << "hugepages active: MAP_HUGETLB mapping obtained";
  } else {
    GTEST_LOG_(INFO)
        << "hugepages unavailable on this host; cleanly fell back to a normal "
           "mapping (this is expected in CI without reserved hugepages)";
  }

  // Regardless of which path we took, the ring must be usable in-process.
  Consumer<R> c(*seg);
  const std::uint32_t v = 0xCAFE;
  seg->publish(&v, sizeof(v));
  auto m = c.read();
  ASSERT_EQ(c.status(), read_status::ok);
  std::uint32_t got = 0;
  std::memcpy(&got, m.data(), m.size());
  EXPECT_EQ(got, v);
  EXPECT_TRUE(c.validate());
}

// ---------------------------------------------------------------------------
// 4. Clean teardown: after a normal creator run, the name is unlinked, so a
//    second shm_open without O_CREAT fails ENOENT. No leaked segment.
// ---------------------------------------------------------------------------
TEST(ShmSegment, CreatorUnlinksOnDestruction) {
  using R = Ring<64, 8, Overwrite>;
  const std::string name = unique_name("teardown");
  pre_unlink(name);

  {
    auto seg = ShmSegment<R>::create(name);
    ASSERT_TRUE(seg.valid());
    // While alive, an opener can find it.
    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    EXPECT_GE(fd, 0) << "segment should exist while creator is alive";
    if (fd >= 0) {
      ::close(fd);
    }
  } // creator destructs -> shm_unlink(name)

  // Now the name must be GONE.
  errno = 0;
  int fd = ::shm_open(name.c_str(), O_RDWR, 0);
  EXPECT_LT(fd, 0) << "segment name leaked after creator teardown";
  EXPECT_EQ(errno, ENOENT);
  if (fd >= 0) {
    ::close(fd);
    ::shm_unlink(name.c_str());
  }
}

// ---------------------------------------------------------------------------
// 4b. An OPENER never unlinks: if the creator outlives an opener, the segment
//     survives the opener's destruction.
// ---------------------------------------------------------------------------
TEST(ShmSegment, OpenerDoesNotUnlink) {
  using R = Ring<64, 8, Overwrite>;
  const std::string name = unique_name("openernounlink");
  pre_unlink(name);

  auto creator = ShmSegment<R>::create(name);
  ASSERT_TRUE(creator.valid());

  {
    auto opener = ShmSegment<R>::open(name);
    ASSERT_TRUE(opener.valid());
    EXPECT_FALSE(opener.is_creator());
  } // opener destructs — must NOT unlink

  // Still present.
  int fd = ::shm_open(name.c_str(), O_RDWR, 0);
  EXPECT_GE(fd, 0) << "opener wrongly unlinked a segment it does not own";
  if (fd >= 0) {
    ::close(fd);
  }
  // creator destructs at end of scope and unlinks.
}

// ---------------------------------------------------------------------------
// open() on a nonexistent name yields a clear OS error (ENOENT), not corruption.
// ---------------------------------------------------------------------------
TEST(ShmSegment, OpenNonexistentThrowsOsError) {
  using R = Ring<64, 8, Overwrite>;
  const std::string name = unique_name("ghost");
  pre_unlink(name);
  EXPECT_THROW({ auto v = ShmSegment<R>::open(name); }, shm_os_error);
}

// ---------------------------------------------------------------------------
// Cross-process Backpressure attach stance: a consumer that joins a Backpressure
// ring mid-stream via attach_consumer_at_current() starts at the producer's
// current published sequence and is lossless FROM THERE, never deadlocking the
// producer. (Documented first-cut stance; full handshake = shimmy-uud.)
// In-process here (no fork) — it exercises the attach primitive's positioning.
// ---------------------------------------------------------------------------
TEST(ShmSegment, BackpressureAttachAtCurrentStartsFromNow) {
  using R = Ring<64, 8, Backpressure, 4>;
  const std::string name = unique_name("bpattach");
  pre_unlink(name);

  auto seg = ShmSegment<R>::create(name);
  ASSERT_TRUE(seg.valid());

  // Producer publishes a few messages BEFORE the late consumer attaches.
  for (std::uint32_t i = 0; i < 3; ++i) {
    const std::uint32_t v = 100u + i;
    seg->publish(&v, sizeof(v));
  }
  ASSERT_EQ(seg->produced(), 3u);

  // Late attach: cursor starts at current published seq (3), NOT 0.
  auto c = seg.attach_consumer_at_current();
  EXPECT_EQ(c.next(), 3u);

  // Nothing readable yet (we attached at the head).
  (void)c.read();
  EXPECT_EQ(c.status(), read_status::empty);

  // New messages from the attach point forward are delivered losslessly.
  const std::uint32_t v = 999u;
  seg->publish(&v, sizeof(v));
  auto m = c.read();
  ASSERT_EQ(c.status(), read_status::ok);
  std::uint32_t got = 0;
  std::memcpy(&got, m.data(), m.size());
  EXPECT_EQ(got, 999u);
  EXPECT_TRUE(c.validate());
  c.commit();
}

} // namespace
