#include "dsidle/shard_allocator.h"
#include "dsidle/latency_simulator.h"
#include "dsidle/swcc_visibility.h"

#include <cstring>
#include <immintrin.h>
#include <new>
#include <stdexcept>
#include <string>

namespace dsidle {
namespace {
constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
inline void FlushSwccLine(const void* address) {
  FlushSwccRange(address, sizeof(FreeObjectHeader));
}

std::uint32_t ClassIndex(std::uint64_t block_size) {
  if (block_size < kSmallestSwccBlock || block_size > kLargestSwccBlock ||
      (block_size & (block_size - 1)))
    throw std::runtime_error("invalid SWCC size class");
  std::uint32_t index = 0;
  for (auto value = block_size; value > kSmallestSwccBlock; value >>= 1) ++index;
  return index;
}

void InitializeClass(SharedPool& pool, std::uint32_t count, std::uint64_t block_size,
                     std::uint64_t range_start, std::uint64_t range_bytes) {
  const auto class_index = ClassIndex(block_size);
  const auto* metadata = pool.static_layout();
  const auto per_shard = (range_bytes / count / block_size) * block_size;
  if (!per_shard) throw std::runtime_error("SWCC capacity too small for shard allocator");
  for (std::uint32_t shard = 0; shard < count; ++shard) {
    auto* entry = new (static_cast<std::byte*>(pool.base()) + metadata->shard_controls_offset +
                       (static_cast<std::uint64_t>(shard) * kSwccSizeClassCount + class_index) * sizeof(ShardControl)) ShardControl{};
    latency_sim::FixedLatencyAtomicStore(
        entry->bump, range_start + shard * per_shard,
        std::memory_order_relaxed, latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::PoolKind::kHwcc, &entry->limit,
        range_start + (shard + 1) * per_shard);
  }
}
}  // namespace

std::uint64_t TaggedFreeListHead::Encode(std::uint64_t offset,
                                         std::uint64_t tag) {
  if ((offset & (kSmallestSwccBlock - 1)) ||
      offset >= kMaximumTaggedSwccPoolBytes)
    throw std::runtime_error("SWCC free-list offset is not tag-encodable");
  if (tag > kMaximumTag)
    throw std::runtime_error("SWCC free-list ABA version is out of range");
  return (tag << kOffsetBits) | (offset >> kOffsetShift);
}

std::uint64_t TaggedFreeListHead::Advance(std::uint64_t old_word,
                                          std::uint64_t new_offset) {
  const auto tag = Tag(old_word);
  if (tag == kMaximumTag)
    throw std::runtime_error("SWCC free-list ABA version exhausted");
  return Encode(new_offset, tag + 1);
}

void FixedBlockShardAllocator::Initialize(SharedPool& pool, std::uint32_t count, std::uint64_t block_size) {
  if (!count || block_size < sizeof(FreeObjectHeader) || (block_size & (block_size - 1)))
    throw std::runtime_error("invalid shard allocator parameters");
  if (latency_sim::FixedLatencyAtomicLoad(
          pool.header()->state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      static_cast<std::uint64_t>(PoolState::kInitializing))
    throw std::runtime_error(
        "SWCC allocator initialization requires INITIALIZING pool state");
  if (pool.size() > kMaximumTaggedSwccPoolBytes)
    throw std::runtime_error("shared pool exceeds tagged SWCC free-list capacity");
  const auto* metadata = pool.static_layout();
  if (metadata->shard_count != count || !metadata->shard_controls_offset)
    throw std::runtime_error("shared-pool shard metadata does not match allocator");
  const auto controls_end = metadata->shard_controls_offset + count * kSwccSizeClassCount * sizeof(ShardControl);
  if (controls_end > pool.header()->hwcc_bytes) throw std::runtime_error("HWCC capacity exhausted by shard metadata");
  const auto start = AlignUp(pool.header()->swcc_offset, block_size);
  const auto usable = pool.header()->swcc_bytes - (start - pool.header()->swcc_offset);
  InitializeClass(pool, count, block_size, start, usable);
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
}

void FixedBlockShardAllocator::InitializeAll(SharedPool& pool, std::uint32_t count) {
  if (!count || pool.static_layout()->shard_count != count)
    throw std::runtime_error("shared-pool shard metadata does not match allocator");
  if (latency_sim::FixedLatencyAtomicLoad(
          pool.header()->state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      static_cast<std::uint64_t>(PoolState::kInitializing))
    throw std::runtime_error(
        "SWCC allocator initialization requires INITIALIZING pool state");
  if (pool.size() > kMaximumTaggedSwccPoolBytes)
    throw std::runtime_error("shared pool exceeds tagged SWCC free-list capacity");
  const auto span = pool.header()->swcc_bytes / kSwccSizeClassCount;
  for (std::uint32_t index = 0; index < kSwccSizeClassCount; ++index) {
    const auto block_size = kSmallestSwccBlock << index;
    const auto raw_start = pool.header()->swcc_offset + static_cast<std::uint64_t>(index) * span;
    const auto start = AlignUp(raw_start, block_size);
    const auto consumed = start - raw_start;
    const auto bytes = span > consumed ? span - consumed : 0;
    InitializeClass(pool, count, block_size, start, bytes);
  }
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
}

FixedBlockShardAllocator::FixedBlockShardAllocator(SharedPool& pool, std::uint32_t count, std::uint64_t block_size)
    : pool_(pool), shard_count_(count), block_size_(block_size), class_index_(ClassIndex(block_size)) {
  const auto shard_count = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool.static_layout()->shard_count);
  if (!count || block_size < sizeof(FreeObjectHeader) || shard_count != count)
    throw std::runtime_error("invalid shard allocator attach");
}

ShardControl* FixedBlockShardAllocator::control(std::uint32_t shard) const {
  if (shard >= shard_count_) throw std::runtime_error("invalid shard index");
  const auto offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc,
      &pool_.static_layout()->shard_controls_offset);
  return reinterpret_cast<ShardControl*>(static_cast<std::byte*>(pool_.base()) + offset +
                                         (static_cast<std::uint64_t>(shard) * kSwccSizeClassCount + class_index_) * sizeof(ShardControl));
}

void FixedBlockShardAllocator::Push(std::atomic<std::uint64_t>& head,
                                    std::uint64_t offset,
                                    std::uint64_t generation) {
  auto* item = reinterpret_cast<FreeObjectHeader*>(static_cast<std::byte*>(pool_.base()) + offset);
  auto old = latency_sim::FixedLatencyAtomicLoad(
      head, std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc);
  do {
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kSwcc,
                                    &item->next_offset,
                                    TaggedFreeListHead::Offset(old));
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kSwcc,
                                    &item->generation, generation);
    FlushSwccLine(item);
    latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
  } while (!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      head, old, TaggedFreeListHead::Advance(old, offset),
      std::memory_order_release, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc));
}

std::uint64_t FixedBlockShardAllocator::Pop(
    std::atomic<std::uint64_t>& head,
    std::atomic<std::uint32_t>& pop_lock) {
  // A tagged head prevents ABA at the CAS, but it cannot protect the
  // preceding dereference of the head object's SWCC next pointer. Another
  // consumer could otherwise pop and reuse that object while this consumer
  // is reading it. Serialize consumers; producers remain lock-free.
  for (;;) {
    while (latency_sim::FixedLatencyAtomicLoad(
               pop_lock, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kHwcc) != 0) {
      _mm_pause();
    }
    std::uint32_t expected = 0;
    if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
            pop_lock, expected, std::uint32_t{1}, std::memory_order_acquire,
            std::memory_order_relaxed, latency_sim::AtomicDomain::kHwcc))
      break;
  }
  struct PopUnlock {
    std::atomic<std::uint32_t>& lock;
    ~PopUnlock() {
      latency_sim::FixedLatencyAtomicStore(lock, 0U, std::memory_order_release,
                                      latency_sim::AtomicDomain::kHwcc);
    }
  } unlock{pop_lock};
  auto old = latency_sim::FixedLatencyAtomicLoad(
      head, std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc);
  while (const auto offset = TaggedFreeListHead::Offset(old)) {
    auto* item = reinterpret_cast<FreeObjectHeader*>(static_cast<std::byte*>(pool_.base()) + offset);
    FlushSwccLine(item);
    latency_sim::FixedLatencyAtomicFence(std::memory_order_acquire,
                                    latency_sim::AtomicDomain::kHwcc);
    const auto next = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::PoolKind::kSwcc, &item->next_offset);
    if (latency_sim::FixedLatencyAtomicCompareExchangeWeak(
            head, old, TaggedFreeListHead::Advance(old, next),
            std::memory_order_acq_rel, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc))
      return offset;
  }
  return 0;
}

std::uint64_t FixedBlockShardAllocator::HarvestRemote(std::uint32_t shard, std::uint64_t maximum) {
  auto* entry = control(shard);
  std::uint64_t harvested = 0;
  while (harvested < maximum) {
    const auto offset =
        Pop(entry->remote_free_head, entry->remote_pop_lock);
    if (!offset) break;
    Push(entry->local_free_head, offset,
         reinterpret_cast<FreeObjectHeader*>(
             static_cast<std::byte*>(pool_.base()) + offset)->generation);
    ++harvested;
  }
  return harvested;
}

SwccOffset<std::byte> FixedBlockShardAllocator::Allocate(std::uint32_t shard) {
  auto* entry = control(shard);
  HarvestRemote(shard);
  if (const auto reused =
          Pop(entry->local_free_head, entry->local_pop_lock))
    return SwccOffset<std::byte>(reused);
  const auto offset = latency_sim::FixedLatencyAtomicFetchAdd(
      entry->bump, block_size_, std::memory_order_acq_rel,
      latency_sim::AtomicDomain::kHwcc);
  const auto limit = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &entry->limit);
  if (offset + block_size_ > limit)
    throw std::runtime_error("D-SIDLE SWCC shard OOM: block=" + std::to_string(block_size_) +
                             " offset=" + std::to_string(offset) +
                             " limit=" + std::to_string(limit));
  std::memset(static_cast<std::byte*>(pool_.base()) + offset, 0, block_size_);
  latency_sim::GlobalLatencySimulator().RecordRange(
      latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kWrite,
      static_cast<std::byte*>(pool_.base()) + offset, block_size_);
  return SwccOffset<std::byte>(offset);
}

void FixedBlockShardAllocator::Free(std::uint32_t owner, SwccOffset<std::byte> block, std::uint64_t generation) {
  if (!block) throw std::runtime_error("cannot free null SWCC offset");
  Push(control(owner)->remote_free_head, block.value(), generation);
}

std::uint64_t SwccShardAllocator::SizeClassBlockSize(std::uint64_t size) {
  if (!size) throw std::runtime_error("cannot allocate zero SWCC bytes");
  auto block = kSmallestSwccBlock;
  while (block < size && block < kLargestSwccBlock) block <<= 1;
  if (block < size) throw std::runtime_error("D-SIDLE SWCC allocation exceeds largest size class");
  return block;
}

SwccOffset<std::byte> SwccShardAllocator::Allocate(std::uint32_t shard, std::uint64_t size) {
  const auto block = SizeClassBlockSize(size);
  return FixedBlockShardAllocator(pool_, shard_count_, block).Allocate(shard);
}

std::uint32_t SwccShardAllocator::OwnerOf(SwccOffset<std::byte> block, std::uint64_t size) const {
  if (!block) throw std::runtime_error("cannot determine owner of null SWCC offset");
  const auto class_index = ClassIndex(SizeClassBlockSize(size));
  const auto block_size = kSmallestSwccBlock << class_index;
  const auto swcc_offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool_.header()->swcc_offset);
  const auto swcc_bytes = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool_.header()->swcc_bytes);
  const auto span = swcc_bytes / kSwccSizeClassCount;
  const auto raw_start = swcc_offset + static_cast<std::uint64_t>(class_index) * span;
  const auto start = AlignUp(raw_start, block_size);
  const auto per_shard = ((span - (start - raw_start)) / shard_count_ / block_size) * block_size;
  for (std::uint32_t shard = 0; shard < shard_count_; ++shard) {
    const auto shard_start = start + static_cast<std::uint64_t>(shard) * per_shard;
    if (block.value() >= shard_start && block.value() < shard_start + per_shard)
      return shard;
  }
  throw std::runtime_error("SWCC offset is outside every shard range");
}

void SwccShardAllocator::Free(std::uint32_t owner_shard, SwccOffset<std::byte> block,
                              std::uint64_t size, std::uint64_t generation) {
  FixedBlockShardAllocator(pool_, shard_count_, SizeClassBlockSize(size)).Free(owner_shard, block, generation);
}

std::uint64_t SwccShardAllocator::HarvestRemote(std::uint32_t shard, std::uint64_t size,
                                                 std::uint64_t maximum) {
  return FixedBlockShardAllocator(pool_, shard_count_, SizeClassBlockSize(size)).HarvestRemote(shard, maximum);
}

}  // namespace dsidle
