#pragma once

#include "dsidle/shared_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dsidle {

constexpr std::uint64_t kSmallestSwccBlock = 64;
constexpr std::uint64_t kLargestSwccBlock = 2ULL << 20;
constexpr std::uint32_t kSwccSizeClassCount = 16;  // 64B .. 2MiB

// Exactly the reusable 16 bytes prescribed for an SWCC free object.
struct FreeObjectHeader {
  std::uint64_t next_offset{};
  std::uint64_t generation{};
};
static_assert(sizeof(FreeObjectHeader) == 16);

// One cache line per shard: the volatile publication heads live in HWCC while
// every object link remains an SWCC offset.
struct alignas(64) ShardControl {
  std::atomic<std::uint64_t> bump{0};
  std::uint64_t limit{0};
  std::atomic<std::uint64_t> local_free_head{0};
  std::atomic<std::uint64_t> remote_free_head{0};
  std::byte padding[32]{};
};
static_assert(sizeof(ShardControl) == 64);

class FixedBlockShardAllocator {
 public:
  // Initialisation is a pool-creation operation and must run once before any
  // process attaches.  Blocks are fixed-size so their free header need not
  // carry an untrusted size field. Pool metadata must already be initialized.
  static void Initialize(SharedPool& pool, std::uint32_t shard_count, std::uint64_t block_size);
  // Initializes every power-of-two size class from 64B through 2MiB. This is
  // the production pool layout; Initialize() remains useful to isolate a
  // single class in focused allocator tests.
  static void InitializeAll(SharedPool& pool, std::uint32_t shard_count);

  FixedBlockShardAllocator(SharedPool& pool, std::uint32_t shard_count, std::uint64_t block_size);
  SwccOffset<std::byte> Allocate(std::uint32_t shard);
  void Free(std::uint32_t owner_shard, SwccOffset<std::byte> block, std::uint64_t generation);
  std::uint64_t HarvestRemote(std::uint32_t shard, std::uint64_t maximum = 64);

 private:
  ShardControl* control(std::uint32_t shard) const;
  void Push(std::atomic<std::uint64_t>& head, std::uint64_t offset, std::uint64_t generation);
  std::uint64_t Pop(std::atomic<std::uint64_t>& head);

  SharedPool& pool_;
  std::uint32_t shard_count_;
  std::uint64_t block_size_;
  std::uint32_t class_index_;
};

// Canonical SWCC allocator used by Masstree objects. The returned address is
// still an offset; callers must not persist the resolved pointer.
class SwccShardAllocator {
 public:
  SwccShardAllocator(SharedPool& pool, std::uint32_t shard_count)
      : pool_(pool), shard_count_(shard_count) {}
  SwccOffset<std::byte> Allocate(std::uint32_t shard, std::uint64_t size);
  std::uint32_t OwnerOf(SwccOffset<std::byte> block, std::uint64_t size) const;
  void Free(std::uint32_t owner_shard, SwccOffset<std::byte> block,
            std::uint64_t size, std::uint64_t generation);
  std::uint64_t HarvestRemote(std::uint32_t shard, std::uint64_t size,
                              std::uint64_t maximum = 64);

  static std::uint64_t SizeClassBlockSize(std::uint64_t size);
 private:
  SharedPool& pool_;
  std::uint32_t shard_count_;
};

}  // namespace dsidle
