#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include "dsidle/latency_simulator.h"
#include "dsidle/swcc_visibility.h"

namespace dsidle {

class SharedPool;

// Process-local mapping base. It is never written to the shared backing; it
// merely resolves persistent offsets at this process's mapping address.
void SetSharedPoolBase(void* base);
void* SharedPoolBase();

template <typename T, typename RegionTag>
class BasicOffset {
 public:
  constexpr BasicOffset() = default;
  explicit constexpr BasicOffset(std::uint64_t value) : value_(value) {}
  constexpr std::uint64_t value() const { return value_; }
  constexpr explicit operator bool() const { return value_ != 0; }
  constexpr T* get(void* pool_base) const {
    return value_ ? reinterpret_cast<T*>(static_cast<std::byte*>(pool_base) + value_) : nullptr;
  }
  friend constexpr bool operator==(BasicOffset left, BasicOffset right) {
    return left.value_ == right.value_;
  }
  friend constexpr bool operator!=(BasicOffset left, BasicOffset right) {
    return !(left == right);
  }

 private:
  std::uint64_t value_{};
};

struct SwccRegion {};
struct HwccRegion {};
template <typename T> using SwccOffset = BasicOffset<T, SwccRegion>;
template <typename T> using HwccOffset = BasicOffset<T, HwccRegion>;

enum class NodeAllocationState : std::uint32_t { kFree, kAllocating, kPublished, kRetiring };
inline constexpr std::size_t kCanonicalNodeEnvelopeBytes = 512;
inline constexpr std::uint32_t kNodeKindBits = 8;

// dsidle: This is the sole cross-VM control cache line for a canonical node.
struct alignas(64) NodeControl {
  std::atomic<std::uint64_t> version_and_state{0};
  std::uint64_t canonical_swcc_offset{0};
  std::uint64_t generation{0};
  std::uint64_t retire_epoch{0};
  std::uint32_t node_type{0};
  std::atomic<NodeAllocationState> allocation_state{NodeAllocationState::kFree};
  std::atomic<std::uint32_t> leaf_link_lock{0};
  std::atomic<std::uint64_t> phantom_epoch{0};
  std::atomic<std::uint64_t> parent_ref{0};
};
static_assert(sizeof(NodeControl) == 64 && alignof(NodeControl) == 64);

using NodeRef = HwccOffset<NodeControl>;
using ValueRef = SwccOffset<std::byte>;

inline NodeAllocationState LoadNodeAllocationState(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    return NodeAllocationState::kFree;
  return latency_sim::FixedLatencyAtomicLoad(
      control->allocation_state, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
}

inline std::uint64_t LoadNodeGeneration(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null generation NodeRef");
  return latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &control->generation);
}

inline std::uint64_t LoadCanonicalSwccOffset(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null canonical NodeRef");
  return latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &control->canonical_swcc_offset);
}

// Compute an opaque canonical address after the caller has validated this
// NodeRef's HWCC generation/version. This does not establish SWCC visibility:
// the returned pointer may be stored for API identity only and must not be
// dereferenced until ResolveCanonicalNode() or an equivalent invalidate.
template <typename T>
inline T* CanonicalNodeAddressFromStableIdentity(NodeRef ref) {
  const auto offset = LoadCanonicalSwccOffset(ref);
  return reinterpret_cast<T*>(
      static_cast<std::byte*>(SharedPoolBase()) + offset);
}

inline std::size_t LoadCanonicalNodeBytes(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null canonical NodeRef");
  const auto bytes = latency_sim::FixedLatencyMemoryLoad(
                         latency_sim::PoolKind::kHwcc, &control->node_type) >>
                     kNodeKindBits;
  // Pools written before exact-size accounting stored only the kind.
  return bytes ? bytes : kCanonicalNodeEnvelopeBytes;
}

inline bool TryLockLeafLink(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null leaf link NodeRef");
  std::uint32_t expected = 0;
  return latency_sim::FixedLatencyAtomicCompareExchangeStrong(
      control->leaf_link_lock, expected, std::uint32_t{1},
      std::memory_order_acquire,
      std::memory_order_relaxed, latency_sim::AtomicDomain::kHwcc);
}

inline void UnlockLeafLink(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null leaf link NodeRef");
  latency_sim::FixedLatencyAtomicStore(control->leaf_link_lock, std::uint32_t{0},
                                  std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
}

inline std::uint64_t LoadNodePhantomEpoch(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null phantom-epoch NodeRef");
  return latency_sim::FixedLatencyAtomicLoad(
      control->phantom_epoch, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
}

inline void StoreNodePhantomEpoch(NodeRef ref, std::uint64_t epoch) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null phantom-epoch NodeRef");
  latency_sim::FixedLatencyAtomicStore(control->phantom_epoch, epoch,
                                  std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
}

inline bool CompareExchangeNodePhantomEpoch(
    NodeRef ref, std::uint64_t* expected, std::uint64_t desired) {
  if (!expected)
    throw std::runtime_error("null phantom-epoch expected value");
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null phantom-epoch NodeRef");
  return latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      control->phantom_epoch, *expected, desired, std::memory_order_acq_rel,
      std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc);
}

inline NodeRef LoadNodeParentRef(NodeRef ref) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null parent NodeRef");
  return NodeRef(latency_sim::FixedLatencyAtomicLoad(
      control->parent_ref, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc));
}

inline void StoreNodeParentRef(NodeRef ref, NodeRef parent) {
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    throw std::runtime_error("null parent NodeRef");
  latency_sim::FixedLatencyAtomicStore(control->parent_ref, parent.value(),
                                  std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
}

// Process-local policy queues carry a persistent control reference plus its
// allocation generation.  A raw canonical address is neither valid across VM
// mappings nor safe after a NodeControl slot has been reused.
struct QueuedNodeRef {
  NodeRef ref{};
  std::uint64_t generation{};

  explicit operator bool() const { return static_cast<bool>(ref) && generation != 0; }
  friend bool operator==(QueuedNodeRef left, QueuedNodeRef right) {
    return left.ref == right.ref && left.generation == right.generation;
  }
};
static_assert(sizeof(QueuedNodeRef) == 16);

// Resolves a canonical tree body only for the duration of the caller's stack
// operation. Persistent structures retain the NodeRef, never this address.
template <typename T>
T* ResolveCanonicalNode(NodeRef ref) {
  if (!ref) return nullptr;
  void* base = SharedPoolBase();
  auto* control = ref.get(base);
  const auto state = control ? LoadNodeAllocationState(ref)
                             : NodeAllocationState::kFree;
  const auto canonical_offset =
      control ? LoadCanonicalSwccOffset(ref) : 0;
  if (!control ||
      (state != NodeAllocationState::kPublished &&
       state != NodeAllocationState::kRetiring) ||
      !canonical_offset)
    throw std::runtime_error("NodeRef does not name a live canonical node");
  T* canonical = SwccOffset<T>(canonical_offset).get(base);
  // NodeControl publishes only the SWCC offset. Invalidate the first line
  // before reading the embedded control_ref_; nodeversion::stable() then
  // invalidates and charges the complete node envelope before traversal.
  InvalidateSwccRange(canonical, kSwccCacheLineBytes);
  latency_sim::GlobalLatencySimulator().RecordRange(
      latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, canonical,
      kSwccCacheLineBytes);
  return canonical;
}

struct StableView {
  std::uint64_t v{};
  std::uint64_t gen{};
  std::uint64_t swcc_off{};
  bool locked() const { return v & 1U; }
};

// The original 32-bit Masstree nodeversion layout remains intact, but its
// storage now lives in NodeControl. These constants intentionally duplicate
// the original bit positions rather than inventing a D-SIDLE-specific state
// machine.
struct MasstreeNodeVersionBits {
  static constexpr std::uint64_t lock_bit = 1U << 0;
  static constexpr std::uint64_t inserting_bit = 1U << 1;
  static constexpr std::uint64_t splitting_bit = 1U << 2;
  static constexpr std::uint64_t dirty_mask = inserting_bit | splitting_bit;
  static constexpr std::uint64_t vinsert_lowbit = 1U << 3;
  static constexpr std::uint64_t vsplit_lowbit = 1U << 9;
  static constexpr std::uint64_t migration_bit = 1U << 28;
  static constexpr std::uint64_t deleted_bit = 1U << 29;
  static constexpr std::uint64_t root_bit = 1U << 30;
  static constexpr std::uint64_t isleaf_bit = 1U << 31;
  static constexpr std::uint64_t split_unlock_mask = ~(root_bit | migration_bit | (vsplit_lowbit - 1));
  static constexpr std::uint64_t unlock_mask = ~(migration_bit | (vinsert_lowbit - 1));
};

struct StableNodeIdentity {
  std::uint64_t generation{};
  std::uint64_t version{};
};

// Read only the HWCC identity needed to validate an immutable local replica.
// This deliberately does not resolve or invalidate the canonical SWCC body.
// A before/after version check closes a concurrent writer window, while the
// allocation-state checks reject a NodeControl that is being retired/reused.
inline bool TryLoadStableNodeIdentity(NodeRef ref, StableNodeIdentity* identity) {
  if (!identity)
    throw std::runtime_error("null stable NodeIdentity output");
  auto* control = ref.get(SharedPoolBase());
  if (!control)
    return false;
  if (latency_sim::FixedLatencyAtomicLoad(
          control->allocation_state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      NodeAllocationState::kPublished)
    return false;
  const auto first = latency_sim::FixedLatencyAtomicLoad(
      control->version_and_state, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  if (first & MasstreeNodeVersionBits::dirty_mask)
    return false;
  const auto generation = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &control->generation);
  const auto second = latency_sim::FixedLatencyAtomicLoad(
      control->version_and_state, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  const auto state = latency_sim::FixedLatencyAtomicLoad(
      control->allocation_state, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  if (first != second ||
      (second & MasstreeNodeVersionBits::dirty_mask) ||
      state != NodeAllocationState::kPublished)
    return false;
  *identity = {generation, second};
  return true;
}

// dsidle: the sole read/write gateway for a canonical node's HWCC control
// line. The canonical offset and generation are read only after the acquire
// version load; writers publish them before an unlocked release store.
class NodeVersionAccessor {
 public:
  NodeVersionAccessor(void* pool_base, NodeRef ref) : pool_base_(pool_base), ref_(ref) {}

  StableView stable() const {
    NodeControl* control = Control(true);
    std::uint64_t version = latency_sim::FixedLatencyAtomicLoad(
        control->version_and_state, std::memory_order_acquire,
        latency_sim::AtomicDomain::kHwcc);
    while (version & MasstreeNodeVersionBits::dirty_mask) {
      version = latency_sim::FixedLatencyAtomicLoad(
          control->version_and_state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
    }
    const auto canonical_offset = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::PoolKind::kHwcc, &control->canonical_swcc_offset);
    const auto generation = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::PoolKind::kHwcc, &control->generation);
    auto* canonical =
        static_cast<std::byte*>(pool_base_) + canonical_offset;
    const auto canonical_bytes = LoadCanonicalNodeBytes(ref_);
    // Match nodeversion::invalidate_canonical's exact-allocation envelope.
    // This accessor is used by control-plane paths that may inspect arbitrary
    // canonical fields after the stable HWCC snapshot.
    InvalidateSwccRange(canonical, canonical_bytes);
    latency_sim::GlobalLatencySimulator().RecordRange(
        latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
        canonical, canonical_bytes);
    return {version, generation, canonical_offset};
  }

  bool try_lock(std::uint64_t* locked_version = nullptr) const {
    NodeControl* control = Control(false);
    auto expected = latency_sim::FixedLatencyAtomicLoad(
        control->version_and_state, std::memory_order_acquire,
        latency_sim::AtomicDomain::kHwcc);
    while (!(expected & (MasstreeNodeVersionBits::lock_bit | MasstreeNodeVersionBits::dirty_mask))) {
      if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
              control->version_and_state, expected,
              expected | MasstreeNodeVersionBits::lock_bit,
              std::memory_order_acq_rel, std::memory_order_acquire,
              latency_sim::AtomicDomain::kHwcc)) {
        const auto canonical_offset = LoadCanonicalSwccOffset(ref_);
        const auto canonical_bytes = LoadCanonicalNodeBytes(ref_);
        InvalidateSwccRange(
            static_cast<std::byte*>(pool_base_) + canonical_offset,
            canonical_bytes);
        latency_sim::GlobalLatencySimulator().RecordRange(
            latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead,
            static_cast<const std::byte*>(pool_base_) + canonical_offset,
            canonical_bytes);
        if (locked_version) *locked_version = expected | MasstreeNodeVersionBits::lock_bit;
        return true;
      }
    }
    return false;
  }

  void mark_insert() const {
    auto* control = Control();
    latency_sim::FixedLatencyAtomicFetchOr(
        control->version_and_state, MasstreeNodeVersionBits::inserting_bit,
        std::memory_order_acq_rel, latency_sim::AtomicDomain::kHwcc);
  }
  void mark_split() const {
    auto* control = Control();
    latency_sim::FixedLatencyAtomicFetchOr(
        control->version_and_state, MasstreeNodeVersionBits::splitting_bit,
        std::memory_order_acq_rel, latency_sim::AtomicDomain::kHwcc);
  }
  void unlock_release(std::uint64_t locked_version) const {
    if (!(locked_version & MasstreeNodeVersionBits::lock_bit)) throw std::runtime_error("NodeControl unlock without lock");
    const auto next = locked_version & MasstreeNodeVersionBits::splitting_bit
        ? (locked_version + MasstreeNodeVersionBits::vsplit_lowbit) & MasstreeNodeVersionBits::split_unlock_mask
        : (locked_version + ((locked_version & MasstreeNodeVersionBits::inserting_bit) << 2)) & MasstreeNodeVersionBits::unlock_mask;
    auto* control = Control();
    latency_sim::FixedLatencyAtomicStore(control->version_and_state, next,
                                    std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
  }

 private:
  NodeControl* Control(bool allow_retiring = false) const {
    auto* control = ref_.get(pool_base_);
    if (!control) throw std::runtime_error("null NodeRef");
    const auto state = latency_sim::FixedLatencyAtomicLoad(
        control->allocation_state, std::memory_order_acquire,
        latency_sim::AtomicDomain::kHwcc);
    if (state != NodeAllocationState::kPublished &&
        !(allow_retiring && state == NodeAllocationState::kRetiring))
      throw std::runtime_error("NodeRef does not name a lockable canonical node");
    return control;
  }
  void* pool_base_;
  NodeRef ref_;
};

struct alignas(64) RootControl {
  std::uint64_t root_ref{0};
  std::uint64_t root_generation{0};
  std::atomic<std::uint64_t> version{0};
  std::byte padding[40]{};
};
static_assert(sizeof(RootControl) == 64 && alignof(RootControl) == 64);

struct RootView {
  NodeRef ref{};
  std::uint64_t generation{};
  std::uint64_t version{};
};

class RootControlAccessor {
 public:
  explicit RootControlAccessor(RootControl* root) : root_(root) {
    if (!root_) throw std::runtime_error("null RootControl");
  }

  RootView stable() const {
    while (true) {
      const auto first = latency_sim::FixedLatencyAtomicLoad(
          root_->version, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
      if (first & 1U)
        continue;
      // Each slot is read exactly once through the HWCC wrapper; the wrapper
      // result is the value used by the seqlock, never a discarded second
      // read of the raw field.
      const auto observed_ref = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &root_->root_ref);
      const auto observed_generation = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &root_->root_generation);
      const auto second = latency_sim::FixedLatencyAtomicLoad(
          root_->version, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
      if (first == second)
        return {NodeRef(observed_ref), observed_generation, second};
    }
  }

  void publish(NodeRef ref, std::uint64_t generation) const {
    if (!ref) throw std::runtime_error("cannot publish null root NodeRef");
    auto version = latency_sim::FixedLatencyAtomicLoad(
        root_->version, std::memory_order_acquire,
        latency_sim::AtomicDomain::kHwcc);
    while (true) {
      if (version & 1U) {
        version = latency_sim::FixedLatencyAtomicLoad(
            root_->version, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc);
        continue;
      }
      if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
              root_->version, version, version + 1,
              std::memory_order_acq_rel, std::memory_order_acquire,
              latency_sim::AtomicDomain::kHwcc))
        break;
    }
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &root_->root_ref, ref.value());
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &root_->root_generation, generation);
    latency_sim::FixedLatencyAtomicStore(root_->version, version + 2,
                                    std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
  }

  bool compare_publish(const RootView& expected, NodeRef ref,
                       std::uint64_t generation) const {
    if (!ref)
      throw std::runtime_error("cannot publish null root NodeRef");
    auto version = expected.version;
    if (version & 1U)
      throw std::runtime_error("cannot compare against unstable root");
    if (!latency_sim::FixedLatencyAtomicCompareExchangeStrong(
            root_->version, version, version + 1,
            std::memory_order_acq_rel, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc))
      return false;
    // The successful seqlock transition proves that the fields still belong
    // to `expected`; a newer root publisher would have changed version.
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &root_->root_ref, ref.value());
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &root_->root_generation, generation);
    latency_sim::FixedLatencyAtomicStore(root_->version, version + 2,
                                    std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
    return true;
  }

 private:
  RootControl* root_;
};

// The free chain uses canonical_swcc_offset only while the slot is FREE; a
// published control line never contains an allocator link.
class NodeControlSlab {
 public:
  explicit NodeControlSlab(SharedPool& pool) : pool_(pool) {}
  // Reserve only claims an HWCC control line.  It intentionally remains
  // ALLOCATING until the caller has constructed and flushed the paired SWCC
  // body, so an incomplete NodeRef can never be published into the tree.
  NodeRef Reserve(std::uint64_t canonical_swcc_offset, std::uint32_t node_type,
                  std::size_t canonical_bytes = kCanonicalNodeEnvelopeBytes);
  // Cancels a reservation that never became visible and returns its HWCC
  // control line to the slab. This is distinct from RETIRING/Release.
  void Cancel(NodeRef ref);
  void Publish(NodeRef ref, std::uint64_t initial_version);
  void Retire(NodeRef ref, std::uint64_t retire_epoch);
  void Release(NodeRef ref);

 private:
  SharedPool& pool_;
};

}  // namespace dsidle
