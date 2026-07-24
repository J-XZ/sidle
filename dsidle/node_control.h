#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include "dsidle/latency_simulator.h"

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

// dsidle: This is the sole cross-VM control cache line for a canonical node.
struct alignas(64) NodeControl {
  std::atomic<std::uint64_t> version_and_state{0};
  std::uint64_t canonical_swcc_offset{0};
  std::uint64_t generation{0};
  std::uint64_t retire_epoch{0};
  std::uint32_t node_type{0};
  std::atomic<NodeAllocationState> allocation_state{NodeAllocationState::kFree};
  std::byte padding[24]{};
};
static_assert(sizeof(NodeControl) == 64 && alignof(NodeControl) == 64);

using NodeRef = HwccOffset<NodeControl>;
using ValueRef = SwccOffset<std::byte>;

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
  if (!control || control->allocation_state.load(std::memory_order_acquire) != NodeAllocationState::kPublished ||
      !control->canonical_swcc_offset)
    throw std::runtime_error("NodeRef does not name a published canonical node");
  T* canonical = SwccOffset<T>(control->canonical_swcc_offset).get(base);
  // Every canonical NodeRef decode starts a node-body traversal. Account the
  // first cache line here; deeper field walks are naturally charged by their
  // value/ksuffix and visibility wrappers.
  latency_sim::RecordSwccRead(canonical, 64);
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

// dsidle: the sole read/write gateway for a canonical node's HWCC control
// line. The canonical offset and generation are read only after the acquire
// version load; writers publish them before an unlocked release store.
class NodeVersionAccessor {
 public:
  NodeVersionAccessor(void* pool_base, NodeRef ref) : pool_base_(pool_base), ref_(ref) {}

  StableView stable() const {
    NodeControl* control = Control();
    latency_sim::RecordHwccAtomicLoad(&control->version_and_state);
    std::uint64_t version = control->version_and_state.load(std::memory_order_acquire);
    while (version & MasstreeNodeVersionBits::dirty_mask)
      version = control->version_and_state.load(std::memory_order_acquire);
    return {version, control->generation, control->canonical_swcc_offset};
  }

  bool try_lock(std::uint64_t* locked_version = nullptr) const {
    NodeControl* control = Control();
    latency_sim::RecordHwccAtomicRmw(&control->version_and_state);
    auto expected = control->version_and_state.load(std::memory_order_acquire);
    while (!(expected & (MasstreeNodeVersionBits::lock_bit | MasstreeNodeVersionBits::dirty_mask))) {
      if (control->version_and_state.compare_exchange_weak(expected, expected | MasstreeNodeVersionBits::lock_bit,
                                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (locked_version) *locked_version = expected | MasstreeNodeVersionBits::lock_bit;
        return true;
      }
    }
    return false;
  }

  void mark_insert() const {
    auto* control = Control();
    latency_sim::RecordHwccAtomicRmw(&control->version_and_state);
    control->version_and_state.fetch_or(MasstreeNodeVersionBits::inserting_bit,
                                        std::memory_order_acq_rel);
  }
  void mark_split() const {
    auto* control = Control();
    latency_sim::RecordHwccAtomicRmw(&control->version_and_state);
    control->version_and_state.fetch_or(MasstreeNodeVersionBits::splitting_bit,
                                        std::memory_order_acq_rel);
  }
  void unlock_release(std::uint64_t locked_version) const {
    if (!(locked_version & MasstreeNodeVersionBits::lock_bit)) throw std::runtime_error("NodeControl unlock without lock");
    const auto next = locked_version & MasstreeNodeVersionBits::splitting_bit
        ? (locked_version + MasstreeNodeVersionBits::vsplit_lowbit) & MasstreeNodeVersionBits::split_unlock_mask
        : (locked_version + ((locked_version & MasstreeNodeVersionBits::inserting_bit) << 2)) & MasstreeNodeVersionBits::unlock_mask;
    auto* control = Control();
    latency_sim::RecordHwccAtomicStore(&control->version_and_state);
    control->version_and_state.store(next, std::memory_order_release);
  }

 private:
  NodeControl* Control() const {
    auto* control = ref_.get(pool_base_);
    if (!control) throw std::runtime_error("null NodeRef");
    if (control->allocation_state.load(std::memory_order_acquire) != NodeAllocationState::kPublished)
      throw std::runtime_error("NodeRef does not name a published canonical node");
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
      latency_sim::RecordHwccAtomicLoad(&root_->version);
      const auto first = root_->version.load(std::memory_order_acquire);
      const NodeRef ref(root_->root_ref);
      const auto generation = root_->root_generation;
      latency_sim::RecordHwccAtomicLoad(&root_->version);
      const auto second = root_->version.load(std::memory_order_acquire);
      if (first == second) return {ref, generation, second};
    }
  }

  void publish(NodeRef ref, std::uint64_t generation) const {
    if (!ref) throw std::runtime_error("cannot publish null root NodeRef");
    root_->root_ref = ref.value();
    root_->root_generation = generation;
    latency_sim::RecordHwccAtomicRmw(&root_->version);
    root_->version.fetch_add(1, std::memory_order_release);
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
  NodeRef Reserve(std::uint64_t canonical_swcc_offset, std::uint32_t node_type);
  void Publish(NodeRef ref, std::uint64_t initial_version);
  void Retire(NodeRef ref, std::uint64_t retire_epoch);
  void Release(NodeRef ref);

 private:
  SharedPool& pool_;
};

}  // namespace dsidle
