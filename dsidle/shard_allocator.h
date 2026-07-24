#pragma once

#include "dsidle/shared_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dsidle {

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
};

}  // namespace dsidle
