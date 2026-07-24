#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dsidle {

class SharedPool;

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
  NodeAllocationState allocation_state{NodeAllocationState::kFree};
  std::byte padding[24]{};
};
static_assert(sizeof(NodeControl) == 64 && alignof(NodeControl) == 64);

using NodeRef = HwccOffset<NodeControl>;
using ValueRef = SwccOffset<std::byte>;

struct alignas(64) RootControl {
  std::uint64_t root_ref{0};
  std::uint64_t root_generation{0};
  std::atomic<std::uint64_t> version{0};
  std::byte padding[40]{};
};
static_assert(sizeof(RootControl) == 64 && alignof(RootControl) == 64);

// The free chain uses canonical_swcc_offset only while the slot is FREE; a
// published control line never contains an allocator link.
class NodeControlSlab {
 public:
  explicit NodeControlSlab(SharedPool& pool) : pool_(pool) {}
  NodeRef Acquire(std::uint64_t canonical_swcc_offset, std::uint32_t node_type);
  void Release(NodeRef ref);

 private:
  SharedPool& pool_;
};

}  // namespace dsidle
