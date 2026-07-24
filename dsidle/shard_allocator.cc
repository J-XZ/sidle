#include "dsidle/shard_allocator.h"

#include <cstring>
#include <immintrin.h>
#include <new>
#include <stdexcept>

namespace dsidle {
namespace {
constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
inline void FlushSwccLine(const void* address) {
  _mm_clflush(address);
}
}  // namespace

void FixedBlockShardAllocator::Initialize(SharedPool& pool, std::uint32_t count, std::uint64_t block_size) {
  if (!count || block_size < sizeof(FreeObjectHeader) || (block_size & (block_size - 1)))
    throw std::runtime_error("invalid shard allocator parameters");
  const auto* metadata = pool.static_layout();
  if (metadata->shard_count != count || !metadata->shard_controls_offset)
    throw std::runtime_error("shared-pool shard metadata does not match allocator");
  const auto controls_end = metadata->shard_controls_offset + count * sizeof(ShardControl);
  if (controls_end > pool.header()->hwcc_bytes) throw std::runtime_error("HWCC capacity exhausted by shard metadata");
  const auto start = AlignUp(pool.header()->swcc_offset, block_size);
  const auto usable = pool.header()->swcc_bytes - (start - pool.header()->swcc_offset);
  const auto per_shard = (usable / count / block_size) * block_size;
  if (!per_shard) throw std::runtime_error("SWCC capacity too small for shard allocator");
  for (std::uint32_t shard = 0; shard < count; ++shard) {
    auto* entry = new (static_cast<std::byte*>(pool.base()) + metadata->shard_controls_offset + shard * sizeof(ShardControl)) ShardControl{};
    entry->bump.store(start + shard * per_shard, std::memory_order_relaxed);
    entry->limit = start + (shard + 1) * per_shard;
  }
  std::atomic_thread_fence(std::memory_order_release);
}

FixedBlockShardAllocator::FixedBlockShardAllocator(SharedPool& pool, std::uint32_t count, std::uint64_t block_size)
    : pool_(pool), shard_count_(count), block_size_(block_size) {
  if (!count || block_size < sizeof(FreeObjectHeader)) throw std::runtime_error("invalid shard allocator attach");
}

ShardControl* FixedBlockShardAllocator::control(std::uint32_t shard) const {
  if (shard >= shard_count_) throw std::runtime_error("invalid shard index");
  return reinterpret_cast<ShardControl*>(static_cast<std::byte*>(pool_.base()) + pool_.static_layout()->shard_controls_offset + shard * sizeof(ShardControl));
}

void FixedBlockShardAllocator::Push(std::atomic<std::uint64_t>& head, std::uint64_t offset, std::uint64_t generation) {
  auto* item = reinterpret_cast<FreeObjectHeader*>(static_cast<std::byte*>(pool_.base()) + offset);
  auto old = head.load(std::memory_order_acquire);
  do {
    item->next_offset = old;
    item->generation = generation;
    FlushSwccLine(item);
    _mm_sfence();
  } while (!head.compare_exchange_weak(old, offset, std::memory_order_release, std::memory_order_acquire));
}

std::uint64_t FixedBlockShardAllocator::Pop(std::atomic<std::uint64_t>& head) {
  auto old = head.load(std::memory_order_acquire);
  while (old) {
    auto* item = reinterpret_cast<FreeObjectHeader*>(static_cast<std::byte*>(pool_.base()) + old);
    FlushSwccLine(item);
    _mm_mfence();
    const auto next = item->next_offset;
    if (head.compare_exchange_weak(old, next, std::memory_order_acq_rel, std::memory_order_acquire)) return old;
  }
  return 0;
}

std::uint64_t FixedBlockShardAllocator::HarvestRemote(std::uint32_t shard, std::uint64_t maximum) {
  auto* entry = control(shard);
  std::uint64_t harvested = 0;
  while (harvested < maximum) {
    const auto offset = Pop(entry->remote_free_head);
    if (!offset) break;
    Push(entry->local_free_head, offset, reinterpret_cast<FreeObjectHeader*>(static_cast<std::byte*>(pool_.base()) + offset)->generation);
    ++harvested;
  }
  return harvested;
}

SwccOffset<std::byte> FixedBlockShardAllocator::Allocate(std::uint32_t shard) {
  auto* entry = control(shard);
  HarvestRemote(shard);
  if (const auto reused = Pop(entry->local_free_head)) return SwccOffset<std::byte>(reused);
  const auto offset = entry->bump.fetch_add(block_size_, std::memory_order_acq_rel);
  if (offset + block_size_ > entry->limit) throw std::runtime_error("D-SIDLE SWCC shard OOM");
  std::memset(static_cast<std::byte*>(pool_.base()) + offset, 0, block_size_);
  return SwccOffset<std::byte>(offset);
}

void FixedBlockShardAllocator::Free(std::uint32_t owner, SwccOffset<std::byte> block, std::uint64_t generation) {
  if (!block) throw std::runtime_error("cannot free null SWCC offset");
  Push(control(owner)->remote_free_head, block.value(), generation);
}

}  // namespace dsidle
