// SPDX-License-Identifier: Apache-2.0
//
// shimmy — shared-memory segment lifecycle (bead shimmy-52e).
//
// This layer places the core Ring (ring.hpp) into a named POSIX shared-memory
// segment so a producer in one process and consumers in OTHER processes can
// share the same channel. The Ring itself is UNCHANGED: it was deliberately
// designed pointer-free, standard-layout, and init()-able over fresh memory
// (see the contract block at the top of ring.hpp), so we may placement-new it
// directly over an mmap'd region and address it identically from every process,
// regardless of the virtual address the segment lands at.
//
// ============================================================================
// On-segment layout
// ============================================================================
//
//   offset 0                      offset header_bytes (== aligned ring offset)
//   ┌──────────────────────────┬─────────────────────────────────────────────┐
//   │ SegmentHeader            │  Ring<...>  (placement-new'd here)           │
//   │ magic, version, params   │  pointer-free, identical bytes per process  │
//   └──────────────────────────┴─────────────────────────────────────────────┘
//
// The header PRECEDES the ring. The ring is placed at the first offset >=
// sizeof(SegmentHeader) that satisfies alignof(Ring) (which is the 64B cache
// line, since the ring's members are alignas(64)). Both creator and opener
// compute this offset the SAME way from compile-time constants, so they agree
// without storing any absolute pointer.
//
// ============================================================================
// Versioned header / compatibility (acceptance criterion 2)
// ============================================================================
//
// The header records a magic number, a layout-version integer, and the Ring's
// COMPILE-TIME parameters (block size, capacity, overflow-policy id,
// max-consumers) plus sizeof/alignof(Ring) and sizeof(Slot). An opener
// validates ITS OWN compile-time Ring instantiation against the bytes already
// in the segment. ANY mismatch — wrong magic, wrong version, different
// BlockSize / Capacity / policy / MaxConsumers, or a struct-size disagreement —
// is rejected with a clear error. We NEVER reinterpret an incompatible layout:
// doing so would read the ring's atomics/slots at the wrong offsets and corrupt
// silently. Fail loud, never corrupt.
//
// ============================================================================
// Teardown / unlink ownership (acceptance criterion 4)
// ============================================================================
//
// The CREATOR owns the name in the filesystem namespace. shm_unlink() removes
// the NAME (so no new opener can find it) but the underlying memory persists
// until the last mapping is munmap'd and the last fd closed — standard POSIX
// shm semantics. A creator destructs by: munmap + close(fd) + shm_unlink(name).
// An opener destructs by: munmap + close(fd) ONLY (it never unlinks a name it
// does not own). After a normal creator run the name is gone, so a later
// shm_open without O_CREAT fails ENOENT — i.e. no leaked segment. Both the
// mapping and the fd are owned by RAII members; an exception thrown mid-create
// unwinds them, so there are no leaks on the error path either.
//
// ============================================================================
// Hugepages (acceptance criterion 3)
// ============================================================================
//
// MAP_HUGETLB is OPT-IN via segment_options::hugepages. POSIX shm objects on
// Linux cannot themselves be backed by hugepages through shm_open/ftruncate;
// the portable, dependency-free way to get a hugepage-backed shared mapping is
// an anonymous MAP_SHARED | MAP_HUGETLB mapping (no fd, shared across fork()).
// That trades away the shm NAME (it is not discoverable by a separate
// shm_open), so the hugepage path here is fork-shared, not name-discoverable.
// If MAP_HUGETLB is requested but unavailable (no hugepages reserved, or the
// kernel rejects the size) we DO NOT hard-fail a normal run: we report it via
// hugepages_active() == false and fall back to a normal shared mapping. The
// tail-latency impact of hugepages is an open question owned by the bench bead
// (shimmy-7d8); this layer only makes the path exist and be selectable.
//
// ============================================================================
// Backpressure CROSS-PROCESS stance (REQUIRED — see shimmy-52e / shimmy-4d4)
// ============================================================================
//
// In-process, the Backpressure policy relies on a START BARRIER: every consumer
// registers its cursor before the producer publishes its Capacity-th message
// (ring.hpp register_consumer() documents the deadlock that a late registrant
// otherwise causes). CROSS-PROCESS that barrier does not exist — a process may
// mmap and start consuming at any instant. This layer therefore takes an
// EXPLICIT, documented stance:
//
//   * OVERWRITE is fully supported cross-process. A late opener simply resyncs
//     to the oldest resident sequence on a detected lap (the policy already
//     handles this), so address-space boundaries change nothing.
//
//   * BACKPRESSURE's lossless guarantee (no consumer ever misses a message) is
//     NOT supported cross-process in this first cut, because we cannot enforce
//     "all consumers joined before the stream started" across processes. To
//     avoid the SILENT PRODUCER DEADLOCK hazard, a cross-process Backpressure
//     consumer must attach via attach_consumer_at_current(): it claims an inline
//     cursor and starts reading at the producer's CURRENT published sequence
//     rather than 0. This guarantees the producer never blocks forever on a
//     cursor stuck behind an already-overwritten slot (the deadlock), at the
//     cost of the late consumer not seeing pre-attach messages — i.e. it is
//     lossless ONLY from the attach point forward. This is the safe, documented
//     behavior; full lossless dynamic-attach with a proper handshake is deferred
//     to shimmy-uud. Callers who need true cross-process losslessness from seq 0
//     must arrange their own out-of-band start barrier (open all consumers,
//     then begin publishing); that is exactly what shimmy-uud will systematize.
//
// ============================================================================

#ifndef SHIMMY_SHM_SEGMENT_HPP
#define SHIMMY_SHM_SEGMENT_HPP

#include <shimmy/policies.hpp>
#include <shimmy/ring.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace shimmy {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

// Thrown when an opener's compile-time Ring instantiation does not match the
// layout recorded in an existing segment's header (or the header is not a
// shimmy segment / is a different layout version). NEVER silently proceed.
class shm_layout_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Thrown for OS-level failures (shm_open/ftruncate/mmap) that are not a layout
// mismatch. Carries errno via std::system_error semantics where available.
class shm_os_error : public std::system_error {
public:
  shm_os_error(int err, const std::string& what)
      : std::system_error(err, std::generic_category(), what) {}
};

// ---------------------------------------------------------------------------
// Overflow-policy id — a stable integer recorded in the header so an opener can
// reject a segment created with a different policy. (Tag types themselves are
// not portable across translation units / processes; an integer is.)
// ---------------------------------------------------------------------------
enum class policy_id : std::uint32_t {
  overwrite = 1,
  backpressure = 2,
};

template <typename Overflow>
constexpr policy_id policy_id_of() noexcept {
  if constexpr (std::is_same_v<Overflow, Overwrite>) {
    return policy_id::overwrite;
  } else {
    static_assert(std::is_same_v<Overflow, Backpressure>,
                  "unknown overflow policy");
    return policy_id::backpressure;
  }
}

// ---------------------------------------------------------------------------
// SegmentHeader — versioned, validated, pointer-free.
// ---------------------------------------------------------------------------
// Lives at offset 0 of the segment. Fixed-width fields only, standard-layout,
// no pointers, so its bytes are identical across processes/compilers. The
// `state` field lets an opener confirm the creator finished constructing the
// ring before it is read (a creator writes it LAST, with release).
struct SegmentHeader {
  // Arbitrary fixed 64-bit sentinel identifying a shimmy segment. The exact
  // value is not meaningful; it just must be stable across versions/processes.
  static constexpr std::uint64_t kMagic = 0x53'48'49'4D'4D'59'7E'01ULL;
  // Bump whenever the header OR the on-segment ring layout protocol changes in
  // a way that makes old and new incompatible.
  static constexpr std::uint32_t kVersion = 1;

  // Creator handshake states for `state` (release-published by the creator).
  static constexpr std::uint32_t kStateInitializing = 0;
  static constexpr std::uint32_t kStateReady = 0x52454459; // 'REDY'

  std::uint64_t magic;          // == kMagic
  std::uint32_t version;        // == kVersion
  std::uint32_t header_bytes;   // offset at which the ring begins (aligned)

  // Ring compile-time parameters — an opener checks each against its OWN
  // instantiation. A mismatch means the two sides would read different bytes.
  std::uint64_t block_size;
  std::uint64_t capacity;
  std::uint32_t policy;         // policy_id
  std::uint32_t max_consumers;

  // Structural fingerprints: catch ABI / padding disagreements that the named
  // params alone would miss (e.g. a future Slot layout change at the same
  // BlockSize). If these differ the layouts are NOT interchangeable.
  std::uint64_t ring_sizeof;
  std::uint64_t ring_alignof;
  std::uint64_t slot_sizeof;

  // Creator -> opener readiness flag. Plain integer; we order it with explicit
  // fences at the call sites (a header field cannot be std::atomic and remain
  // trivially memcpy-comparable, and we want the header POD).
  std::uint32_t state;
  std::uint32_t reserved; // pad to 8-byte multiple; keep deterministic bytes
};

static_assert(std::is_standard_layout_v<SegmentHeader>,
              "SegmentHeader must be standard-layout (shared across processes)");
static_assert(std::is_trivially_copyable_v<SegmentHeader>,
              "SegmentHeader must be trivially copyable");

// ---------------------------------------------------------------------------
// segment_options — knobs for create().
// ---------------------------------------------------------------------------
struct segment_options {
  // Request hugepage backing (MAP_HUGETLB). Best-effort: falls back to a normal
  // mapping if unavailable. See the hugepage note at the top of this file.
  bool hugepages = false;
  // Permissions for the shm object (creator path). 0600 = owner only.
  mode_t mode = 0600;
};

namespace detail {

// Round `v` up to the next multiple of `align` (which must be a power of two).
constexpr std::size_t align_up(std::size_t v, std::size_t align) noexcept {
  return (v + (align - 1)) & ~(align - 1);
}

// Offset at which the Ring is placed: the first offset >= sizeof(SegmentHeader)
// that satisfies alignof(RingT). Computed identically by creator and opener
// from compile-time constants — no stored pointer, address-independent.
template <typename RingT>
constexpr std::size_t ring_offset() noexcept {
  return align_up(sizeof(SegmentHeader), alignof(RingT));
}

// Total bytes the segment must hold for ring RingT.
template <typename RingT>
constexpr std::size_t segment_bytes() noexcept {
  return ring_offset<RingT>() + sizeof(RingT);
}

[[noreturn]] inline void throw_os(const char* what) {
  const int err = errno;
  throw shm_os_error(err, std::string(what) + ": " + std::strerror(err));
}

// Format a 64-bit value as a fixed-width, upper-case, 16-digit hex string. Used
// only on the cold layout-error path (e.g. reporting a bad magic).
inline std::string to_hex(std::uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX",
                static_cast<unsigned long long>(v));
  return std::string(buf);
}

// Throw shm_layout_error if `got` != `want`, naming the field. Used by header
// validation to reject a segment whose recorded layout differs from this build.
template <typename A, typename B>
inline void check_eq(const char* field, A got, B want) {
  if (static_cast<std::uint64_t>(got) != static_cast<std::uint64_t>(want)) {
    throw shm_layout_error(
        std::string("incompatible ") + field + ": segment=" +
        std::to_string(static_cast<std::uint64_t>(got)) + " this build=" +
        std::to_string(static_cast<std::uint64_t>(want)));
  }
}

} // namespace detail

// ---------------------------------------------------------------------------
// ShmSegment<RingT> — RAII owner of one named shared-memory segment + its Ring.
// ---------------------------------------------------------------------------
// Construct via the static create()/open() factories. The object owns the fd,
// the mapping, and (if creator) the name. Destruction munmaps, closes, and —
// for a creator — shm_unlinks. Move-only.
template <typename RingT>
class ShmSegment {
public:
  using ring_type = RingT;

  ShmSegment() = default;

  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;

  ShmSegment(ShmSegment&& other) noexcept { swap(other); }
  ShmSegment& operator=(ShmSegment&& other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }

  ~ShmSegment() { reset(); }

  // --- Factories -----------------------------------------------------------

  // Create (or truncate-and-reinitialize) a named segment, size it, map it, and
  // construct a fresh Ring in it. The caller becomes the OWNER and will
  // shm_unlink on destruction. `name` follows POSIX shm rules (leading '/').
  static ShmSegment create(const std::string& name,
                           const segment_options& opts = {}) {
#if !defined(__linux__)
    (void)name;
    (void)opts;
    throw shm_os_error(ENOSYS, "shimmy shm segments are Linux-only");
#else
    ShmSegment seg;
    seg.name_ = name;
    seg.owns_name_ = true;

    const std::size_t bytes = detail::segment_bytes<RingT>();

    // Try the hugepage path first if requested; on failure fall back cleanly.
    if (opts.hugepages) {
      if (seg.try_create_hugetlb(bytes)) {
        seg.construct_ring_and_publish_header();
        return seg;
      }
      // Fall through to the normal shm path (hugepages_active stays false).
    }

    // Normal POSIX shm path: shm_open(O_CREAT) -> ftruncate -> mmap.
    // O_TRUNC so a stale segment of the same name is reset to a clean state.
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR | O_TRUNC,
                              opts.mode);
    if (fd < 0) {
      detail::throw_os("shm_open(O_CREAT)");
    }
    seg.fd_ = fd;

    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      detail::throw_os("ftruncate");
    }

    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
      detail::throw_os("mmap");
    }
    seg.map_ = p;
    seg.map_bytes_ = bytes;

    seg.construct_ring_and_publish_header();
    return seg;
#endif
  }

  // Open an EXISTING named segment created by another process, validate its
  // header against THIS instantiation of RingT, and return a view. Does NOT own
  // the name (will not unlink). Throws shm_layout_error on any incompatibility,
  // shm_os_error on OS failure (e.g. ENOENT if the segment does not exist).
  static ShmSegment open(const std::string& name) {
#if !defined(__linux__)
    (void)name;
    throw shm_os_error(ENOSYS, "shimmy shm segments are Linux-only");
#else
    ShmSegment seg;
    seg.name_ = name;
    seg.owns_name_ = false;

    const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) {
      detail::throw_os("shm_open(open existing)");
    }
    seg.fd_ = fd;

    // Confirm the file is at least large enough to hold the header before we
    // map and read it. (A creator ftruncates to the full size up front.)
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      detail::throw_os("fstat");
    }
    const std::size_t expected = detail::segment_bytes<RingT>();
    if (static_cast<std::size_t>(st.st_size) < sizeof(SegmentHeader)) {
      throw shm_layout_error(
          "segment too small to contain a shimmy header (size=" +
          std::to_string(st.st_size) + ")");
    }

    void* p = ::mmap(nullptr, expected, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                     0);
    if (p == MAP_FAILED) {
      detail::throw_os("mmap(open existing)");
    }
    seg.map_ = p;
    seg.map_bytes_ = expected;

    // Validate BEFORE exposing the ring. Throws on mismatch; the RAII members
    // unwind (munmap + close) so we never leak on the rejection path.
    seg.validate_header(static_cast<std::size_t>(st.st_size));
    return seg;
#endif
  }

  // --- Accessors -----------------------------------------------------------

  // The Ring living in the segment. Same object (by bytes) in every process.
  RingT* ring() noexcept { return ring_; }
  const RingT* ring() const noexcept { return ring_; }

  RingT& operator*() noexcept { return *ring_; }
  RingT* operator->() noexcept { return ring_; }

  bool valid() const noexcept { return ring_ != nullptr; }
  bool is_creator() const noexcept { return owns_name_; }
  bool hugepages_active() const noexcept { return hugepages_active_; }
  const std::string& name() const noexcept { return name_; }
  std::size_t mapped_bytes() const noexcept { return map_bytes_; }

  // --- Cross-process Backpressure attach (documented stance, see file header) -
  //
  // Register a consumer cursor and START it at the producer's CURRENT published
  // sequence rather than 0, then return a Consumer<RingT> positioned there. Use
  // this from a SEPARATE process for a Backpressure ring to avoid the
  // late-registration producer deadlock. The returned consumer is lossless from
  // the attach point forward (it will not see pre-attach messages). For
  // Overwrite this is unnecessary (just construct a Consumer and resync on lap).
  Consumer<RingT> attach_consumer_at_current() noexcept {
    Consumer<RingT> c(*ring_);
    const std::uint64_t cur = ring_->produced();
    c.seek(cur);
    return c;
  }

  // Explicitly drop ownership without unlinking (e.g. hand the name to another
  // owner). After this the destructor will not shm_unlink.
  void release_name_ownership() noexcept { owns_name_ = false; }

private:
#if defined(__linux__)
  // Place a fresh Ring at the aligned offset and publish a valid, READY header.
  // Used by BOTH the shm and hugepage create paths.
  void construct_ring_and_publish_header() noexcept {
    auto* base = static_cast<std::byte*>(map_);
    auto* hdr = std::launder(reinterpret_cast<SegmentHeader*>(base));

    // Write the header as still-initializing first; only flip to READY after the
    // ring is fully constructed and a release fence has ordered those writes.
    std::memset(base, 0, sizeof(SegmentHeader));
    hdr->magic = SegmentHeader::kMagic;
    hdr->version = SegmentHeader::kVersion;
    hdr->header_bytes = static_cast<std::uint32_t>(detail::ring_offset<RingT>());
    hdr->block_size = RingT::block_size;
    hdr->capacity = RingT::capacity;
    hdr->policy =
        static_cast<std::uint32_t>(policy_id_of<typename RingT::overflow_policy>());
    hdr->max_consumers = static_cast<std::uint32_t>(RingT::max_consumers);
    hdr->ring_sizeof = sizeof(RingT);
    hdr->ring_alignof = alignof(RingT);
    hdr->slot_sizeof = sizeof(typename RingT::slot_type);
    hdr->state = SegmentHeader::kStateInitializing;

    // Placement-new the Ring over the (zeroed) mapping. Ring's default ctor
    // calls init(), which resets all stamps/cursors and issues a trailing
    // release fence. The ring is pointer-free, so this is the SAME bytes any
    // opener will see.
    void* ring_mem = base + detail::ring_offset<RingT>();
    ring_ = ::new (ring_mem) RingT();

    // Order all of the above (header fields + ring construction) BEFORE the
    // READY publish, so an opener that observes kStateReady also observes a
    // fully-initialized header and ring.
    std::atomic_thread_fence(std::memory_order_release);
    hdr->state = SegmentHeader::kStateReady;
  }

  // Validate an existing segment's header against THIS RingT. Throws
  // shm_layout_error on any mismatch. On success sets ring_.
  void validate_header(std::size_t file_size) {
    auto* base = static_cast<std::byte*>(map_);
    const auto* hdr =
        std::launder(reinterpret_cast<const SegmentHeader*>(base));

    if (hdr->magic != SegmentHeader::kMagic) {
      throw shm_layout_error("bad magic: not a shimmy segment (got 0x" +
                             detail::to_hex(hdr->magic) + ")");
    }
    if (hdr->version != SegmentHeader::kVersion) {
      throw shm_layout_error("layout version mismatch: segment=" +
                             std::to_string(hdr->version) + " expected=" +
                             std::to_string(SegmentHeader::kVersion));
    }
    // An acquire fence pairs with the creator's release before kStateReady, so
    // if we read READY we also see the fully-initialized fields below.
    if (hdr->state != SegmentHeader::kStateReady) {
      throw shm_layout_error(
          "segment not ready (creator still initializing or crashed mid-init)");
    }
    std::atomic_thread_fence(std::memory_order_acquire);

    detail::check_eq("block_size", hdr->block_size, RingT::block_size);
    detail::check_eq("capacity", hdr->capacity, RingT::capacity);
    detail::check_eq("policy", hdr->policy,
                     static_cast<std::uint32_t>(
                         policy_id_of<typename RingT::overflow_policy>()));
    detail::check_eq("max_consumers", hdr->max_consumers,
                     static_cast<std::uint32_t>(RingT::max_consumers));
    detail::check_eq("ring_sizeof", hdr->ring_sizeof, sizeof(RingT));
    detail::check_eq("ring_alignof", hdr->ring_alignof, alignof(RingT));
    detail::check_eq("slot_sizeof", hdr->slot_sizeof,
                     sizeof(typename RingT::slot_type));
    detail::check_eq("header_bytes", hdr->header_bytes,
                     static_cast<std::uint32_t>(detail::ring_offset<RingT>()));

    // The recorded layout matches; confirm the file is big enough to hold the
    // whole ring at the agreed offset (guards a truncated/corrupt segment).
    if (file_size < detail::segment_bytes<RingT>()) {
      throw shm_layout_error(
          "segment file smaller than required (" + std::to_string(file_size) +
          " < " + std::to_string(detail::segment_bytes<RingT>()) + ")");
    }

    // Layout agreed: adopt the already-constructed ring in place. We do NOT
    // placement-new here (that would re-init and stomp live data); we launder a
    // pointer to the existing object. Its lifetime began in the creator.
    void* ring_mem = base + detail::ring_offset<RingT>();
    ring_ = std::launder(reinterpret_cast<RingT*>(ring_mem));
  }

  // Best-effort hugepage create. Returns true if a MAP_HUGETLB mapping was
  // obtained (and sets the mapping members). Returns false to signal the caller
  // to fall back to the normal shm path. Never throws on the "unavailable"
  // case — only a hard, unexpected failure should surface, and we still prefer
  // graceful fallback for a normal run, so we just report false.
  bool try_create_hugetlb(std::size_t bytes) noexcept {
#if defined(MAP_HUGETLB)
    // Round up to the (default) hugepage size; an unrounded length is rejected
    // by the kernel. We don't know the exact huge size portably, so round to a
    // conservative 2 MiB boundary (the common x86-64 default hugepage).
    constexpr std::size_t huge = std::size_t{2} * 1024 * 1024;
    const std::size_t rounded = detail::align_up(bytes, huge);
    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
      return false; // hugepages unavailable -> caller falls back.
    }
    map_ = p;
    map_bytes_ = rounded;
    hugepages_active_ = true;
    // No fd/name backing for an anonymous hugepage mapping: it is shared via
    // fork(), not via shm_open. owns_name_ stays true only for unlink bookkeep-
    // ing of a name we may never have created; clear it so we don't try to
    // unlink a nonexistent name on teardown.
    owns_name_ = false;
    return true;
#else
    (void)bytes;
    return false;
#endif
  }
#endif // __linux__

  void swap(ShmSegment& o) noexcept {
    std::swap(ring_, o.ring_);
    std::swap(map_, o.map_);
    std::swap(map_bytes_, o.map_bytes_);
    std::swap(fd_, o.fd_);
    std::swap(owns_name_, o.owns_name_);
    std::swap(hugepages_active_, o.hugepages_active_);
    name_.swap(o.name_);
  }

  // RAII teardown: ring lifetime ends (trivial dtor — Ring has no resources),
  // unmap, close fd, and unlink the name IF we are the creator. Idempotent.
  void reset() noexcept {
#if defined(__linux__)
    if (map_ != nullptr) {
      // Ring's destructor is trivial (= default, no owned resources); we may
      // simply unmap. We do not call ring_->~RingT() because the storage is
      // about to be unmapped and the dtor is a no-op anyway.
      ::munmap(map_, map_bytes_);
      map_ = nullptr;
      map_bytes_ = 0;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    if (owns_name_ && !name_.empty()) {
      ::shm_unlink(name_.c_str());
    }
#endif
    ring_ = nullptr;
    owns_name_ = false;
    hugepages_active_ = false;
    name_.clear();
  }

  RingT* ring_ = nullptr;
  void* map_ = nullptr;
  std::size_t map_bytes_ = 0;
  int fd_ = -1;
  bool owns_name_ = false;
  bool hugepages_active_ = false;
  std::string name_;
};

} // namespace shimmy

#endif // SHIMMY_SHM_SEGMENT_HPP
