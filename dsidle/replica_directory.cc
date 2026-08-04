#include "dsidle/replica_directory.h"

#include <cstdlib>
#include <immintrin.h>
#include <stdexcept>

namespace dsidle {
namespace {
thread_local ReplicaDirectory* current_replica_directory = nullptr;
}

void ConfigureCurrentReplicaDirectory(ReplicaDirectory& directory) {
  current_replica_directory = &directory;
}

ReplicaDirectory* CurrentReplicaDirectoryOrNull() { return current_replica_directory; }

ReplicaDirectory::ReplicaDirectory(const SharedPool& pool)
    : node_control_offset_(pool.static_layout()->node_control_offset),
      capacity_(pool.static_layout()->node_control_capacity),
      segment_count_((capacity_ + kSlotsPerSegment - 1) / kSlotsPerSegment),
      segments_(std::make_unique<std::atomic<Segment*>[]>(segment_count_)) {
  if (!node_control_offset_ || !capacity_)
    throw std::runtime_error("ReplicaDirectory requires initialized NodeControl metadata");
  for (std::uint64_t index = 0; index < segment_count_; ++index)
    segments_[index].store(static_cast<Segment*>(nullptr),
                           std::memory_order_relaxed);
}

ReplicaDirectory::~ReplicaDirectory() {
  if (current_replica_directory == this) current_replica_directory = nullptr;
  for (std::uint64_t index = 0; index < segment_count_; ++index) {
    Segment* segment =
        segments_[index].load(std::memory_order_relaxed);
    if (!segment) continue;
    for (Slot& slot : segment->slots)
      std::free(slot.local_ptr.load(std::memory_order_relaxed));
    delete segment;
  }
}

ReplicaDirectory::ReadHandle::ReadHandle(ReadHandle&& other) noexcept
    : slot_(other.slot_), snapshot_(other.snapshot_) { other.slot_ = nullptr; }
ReplicaDirectory::ReadHandle& ReplicaDirectory::ReadHandle::operator=(ReadHandle&& other) noexcept {
  if (this != &other) { Reset(); slot_ = other.slot_; snapshot_ = other.snapshot_; other.slot_ = nullptr; }
  return *this;
}
ReplicaDirectory::ReadHandle::~ReadHandle() { Reset(); }
void ReplicaDirectory::ReadHandle::Reset() {
  if (slot_)
    slot_->readers.fetch_sub(std::uint32_t{1}, std::memory_order_release);
  slot_ = nullptr;
}

std::uint64_t ReplicaDirectory::Index(NodeRef ref) const {
  if (!ref || ref.value() < node_control_offset_ ||
      (ref.value() - node_control_offset_) % sizeof(NodeControl))
    throw std::runtime_error("invalid NodeRef for ReplicaDirectory");
  const auto index = (ref.value() - node_control_offset_) / sizeof(NodeControl);
  if (index >= capacity_) throw std::runtime_error("NodeRef exceeds ReplicaDirectory capacity");
  return index;
}

ReplicaDirectory::Slot* ReplicaDirectory::Find(NodeRef ref) const {
  const auto index = Index(ref);
  Segment* segment =
      segments_[index / kSlotsPerSegment].load(std::memory_order_acquire);
  return segment ? &segment->slots[index % kSlotsPerSegment] : nullptr;
}

ReplicaDirectory::Slot* ReplicaDirectory::Ensure(NodeRef ref) const {
  const auto index = Index(ref);
  auto& published = segments_[index / kSlotsPerSegment];
  Segment* segment = published.load(std::memory_order_acquire);
  if (!segment) {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    segment = published.load(std::memory_order_relaxed);
    if (!segment) {
      segment = new Segment{};
      published.store(segment, std::memory_order_release);
    }
  }
  return &segment->slots[index % kSlotsPerSegment];
}

void ReplicaDirectory::WaitForReaders(Slot& slot) {
  while (slot.readers.load(std::memory_order_acquire) != 0)
    _mm_pause();
}

ReplicaDirectory::ReadHandle ReplicaDirectory::Acquire(NodeRef ref, std::uint64_t generation,
                                                        std::uint64_t cached_version) {
  Slot* slot = Find(ref);
  if (!slot) return {};
  while (true) {
    slot->readers.fetch_add(std::uint32_t{1}, std::memory_order_acquire);
    const auto first = slot->seq.load(std::memory_order_acquire);
    if (first & 1) {
      slot->readers.fetch_sub(std::uint32_t{1}, std::memory_order_release);
      continue;
    }
    ReplicaSnapshot snapshot{
        slot->local_ptr.load(std::memory_order_relaxed),
        slot->generation.load(std::memory_order_relaxed),
        slot->cached_version.load(std::memory_order_relaxed),
        slot->bytes.load(std::memory_order_relaxed),
        static_cast<ReplicaKind>(slot->kind.load(std::memory_order_relaxed))};
    const auto second = slot->seq.load(std::memory_order_acquire);
    if (first == second && !(second & 1) && snapshot.local_ptr &&
        snapshot.generation == generation && snapshot.cached_version == cached_version)
      return ReadHandle(slot, snapshot);
    slot->readers.fetch_sub(std::uint32_t{1}, std::memory_order_release);
    if (first == second && !(second & 1)) {
      std::lock_guard<std::mutex> lock(budget_mutex_);
      std::uint64_t stale_bytes = 0;
      void* stale = InvalidateOlderLocked(
          ref, generation, cached_version, &stale_bytes);
      if (stale) {
        local_bytes_.fetch_sub(stale_bytes, std::memory_order_release);
        std::free(stale);
      }
      return {};
    }
  }
}

bool ReplicaDirectory::HasLocalPlacement(
    NodeRef ref, std::uint64_t generation) const {
  Slot* slot = Find(ref);
  if (!slot) return false;
  while (true) {
    const auto first = slot->seq.load(std::memory_order_acquire);
    if (first & 1) {
      _mm_pause();
      continue;
    }
    const auto desired =
        slot->desired_local.load(std::memory_order_relaxed);
    const auto slot_generation =
        slot->generation.load(std::memory_order_relaxed);
    const auto second = slot->seq.load(std::memory_order_acquire);
    if (first == second)
      return desired && slot_generation == generation;
  }
}

void* ReplicaDirectory::PublishLocked(NodeRef ref, ReplicaSnapshot snapshot) {
  if (!snapshot.local_ptr || !snapshot.generation || !snapshot.bytes)
    throw std::runtime_error("invalid ReplicaDirectory publication");
  Slot& slot = *Ensure(ref);
  auto sequence = slot.seq.load(std::memory_order_acquire);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = slot.seq.load(std::memory_order_acquire);
      continue;
    }
    if (slot.seq.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acq_rel, std::memory_order_acquire))
      break;
  }
  WaitForReaders(slot);
  void* old = slot.local_ptr.load(std::memory_order_relaxed);
  slot.generation.store(snapshot.generation, std::memory_order_relaxed);
  slot.cached_version.store(snapshot.cached_version, std::memory_order_relaxed);
  slot.bytes.store(snapshot.bytes, std::memory_order_relaxed);
  slot.kind.store(static_cast<std::uint32_t>(snapshot.kind), std::memory_order_relaxed);
  slot.local_ptr.store(snapshot.local_ptr, std::memory_order_relaxed);
  slot.desired_local.store(true, std::memory_order_relaxed);
  slot.seq.store(sequence + 2, std::memory_order_release);
  return old;
}

void* ReplicaDirectory::Publish(NodeRef ref, ReplicaSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  auto old_bytes =
      prior ? prior->bytes.load(std::memory_order_relaxed)
            : 0;
  void* old = PublishLocked(ref, snapshot);
  local_bytes_.fetch_add(snapshot.bytes - old_bytes, std::memory_order_release);
  return old;
}

bool ReplicaDirectory::TryPublish(NodeRef ref, ReplicaSnapshot snapshot, void** superseded) {
  if (!superseded) throw std::runtime_error("null ReplicaDirectory superseded output");
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  auto old_bytes =
      prior ? prior->bytes.load(std::memory_order_relaxed)
            : 0;
  auto current = local_bytes_.load(std::memory_order_relaxed);
  const auto budget = budget_bytes_.load(std::memory_order_relaxed);
  if (snapshot.bytes > budget)
    return false;
  if (current - old_bytes > budget - snapshot.bytes) {
    ReclaimRetiredLocked();
    current = local_bytes_.load(std::memory_order_relaxed);
    prior = Find(ref);
    old_bytes =
        prior ? prior->bytes.load(std::memory_order_relaxed)
              : 0;
    if (current - old_bytes > budget - snapshot.bytes)
      return false;
  }
  *superseded = PublishLocked(ref, snapshot);
  local_bytes_.store(current - old_bytes + snapshot.bytes, std::memory_order_release);
  return true;
}

bool ReplicaDirectory::TryRefresh(NodeRef ref, ReplicaSnapshot snapshot,
                                  bool budgeted, void** superseded) {
  if (!superseded)
    throw std::runtime_error("null ReplicaDirectory superseded output");
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  if (!prior ||
      !prior->desired_local.load(std::memory_order_relaxed) ||
      prior->generation.load(std::memory_order_relaxed) !=
          snapshot.generation)
    return false;
  const auto old_bytes =
      prior->bytes.load(std::memory_order_relaxed);
  auto current = local_bytes_.load(std::memory_order_relaxed);
  const auto budget = budget_bytes_.load(std::memory_order_relaxed);
  if (budgeted) {
    if (snapshot.bytes > budget)
      return false;
    if (current - old_bytes > budget - snapshot.bytes) {
      ReclaimRetiredLocked();
      current = local_bytes_.load(std::memory_order_relaxed);
      if (current - old_bytes > budget - snapshot.bytes)
        return false;
    }
  }
  *superseded = PublishLocked(ref, snapshot);
  local_bytes_.store(current - old_bytes + snapshot.bytes, std::memory_order_release);
  return true;
}

void* ReplicaDirectory::InvalidateLocked(NodeRef ref) {
  Slot* slot = Find(ref);
  if (!slot) return nullptr;
  auto sequence = slot->seq.load(std::memory_order_acquire);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = slot->seq.load(std::memory_order_acquire);
      continue;
    }
    if (slot->seq.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acq_rel, std::memory_order_acquire))
      break;
  }
  WaitForReaders(*slot);
  void* old = slot->local_ptr.exchange(static_cast<void*>(nullptr), std::memory_order_relaxed);
  slot->cached_version.store(std::uint64_t{0}, std::memory_order_relaxed);
  slot->bytes.store(std::uint64_t{0}, std::memory_order_relaxed);
  slot->kind.store(std::uint32_t{0}, std::memory_order_relaxed);
  slot->generation.store(std::uint64_t{0}, std::memory_order_relaxed);
  slot->desired_local.store(false, std::memory_order_relaxed);
  slot->seq.store(sequence + 2, std::memory_order_release);
  return old;
}

void ReplicaDirectory::ReclaimRetiredLocked() {
  for (std::uint64_t segment_index = 0;
       segment_index < segment_count_; ++segment_index) {
    Segment* segment = segments_[segment_index].load(std::memory_order_acquire);
    if (!segment)
      continue;
    for (std::uint64_t slot_index = 0;
         slot_index < kSlotsPerSegment; ++slot_index) {
      const auto index =
          segment_index * kSlotsPerSegment + slot_index;
      if (index >= capacity_)
        return;
      Slot& slot = segment->slots[slot_index];
      const auto generation =
          slot.generation.load(std::memory_order_relaxed);
      if (!generation &&
          !slot.local_ptr.load(std::memory_order_relaxed))
        continue;
      const NodeRef ref(
          node_control_offset_ + index * sizeof(NodeControl));
      auto* control = ref.get(SharedPoolBase());
      const auto state = latency_sim::FixedLatencyAtomicLoad(
          control->allocation_state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
      const auto control_generation = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &control->generation);
      if (state == NodeAllocationState::kPublished &&
          control_generation == generation)
        continue;
      const auto bytes = slot.bytes.load(std::memory_order_relaxed);
      void* stale = InvalidateLocked(ref);
      if (stale) {
        local_bytes_.fetch_sub(bytes, std::memory_order_release);
        std::free(stale);
      }
    }
  }
}

void* ReplicaDirectory::InvalidateOlderLocked(
    NodeRef ref, std::uint64_t generation, std::uint64_t cached_version,
    std::uint64_t* removed_bytes) {
  if (!removed_bytes)
    throw std::runtime_error("null stale replica byte output");
  *removed_bytes = 0;
  Slot* slot = Find(ref);
  if (!slot) return nullptr;
  auto sequence = slot->seq.load(std::memory_order_acquire);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = slot->seq.load(std::memory_order_acquire);
      continue;
    }
    if (slot->seq.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acq_rel, std::memory_order_acquire))
      break;
  }
  // Lazy pruning is opportunistic: unlike an explicit publisher/evictor it
  // must not stall a foreground miss behind another valid ReadHandle.
  if (slot->readers.load(std::memory_order_acquire) != 0) {
    slot->seq.store(sequence + 2, std::memory_order_release);
    return nullptr;
  }
  auto* control = ref.get(SharedPoolBase());
  const auto state = control ? LoadNodeAllocationState(ref)
                             : NodeAllocationState::kFree;
  const auto canonical_version = control
      ? latency_sim::FixedLatencyAtomicLoad(
            control->version_and_state, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc)
      : 0;
  if (!control ||
      (state != NodeAllocationState::kPublished &&
       state != NodeAllocationState::kRetiring) ||
      LoadNodeGeneration(ref) != generation ||
      canonical_version != cached_version) {
    slot->seq.store(sequence + 2, std::memory_order_release);
    return nullptr;
  }
  const auto slot_generation = slot->generation.load(std::memory_order_relaxed);
  const auto slot_version = slot->cached_version.load(std::memory_order_relaxed);
  if (slot_generation == generation && slot_version == cached_version) {
    slot->seq.store(sequence + 2, std::memory_order_release);
    return nullptr;
  }
  *removed_bytes = slot->bytes.load(std::memory_order_relaxed);
  void* stale = slot->local_ptr.exchange(static_cast<void*>(nullptr), std::memory_order_relaxed);
  slot->cached_version.store(std::uint64_t{0}, std::memory_order_relaxed);
  slot->bytes.store(std::uint64_t{0}, std::memory_order_relaxed);
  slot->kind.store(std::uint32_t{0}, std::memory_order_relaxed);
  if (slot_generation != generation) {
    slot->generation.store(std::uint64_t{0}, std::memory_order_relaxed);
    slot->desired_local.store(false, std::memory_order_relaxed);
  }
  slot->seq.store(sequence + 2, std::memory_order_release);
  return stale;
}

void* ReplicaDirectory::Invalidate(NodeRef ref) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* slot = Find(ref);
  const auto old_bytes = slot ? slot->bytes.load(std::memory_order_relaxed)
                              : 0;
  void* old = InvalidateLocked(ref);
  if (old)
    local_bytes_.fetch_sub(old_bytes, std::memory_order_release);
  return old;
}

void* ReplicaDirectory::ResetForReuse(NodeRef ref) {
  Slot& slot = *Ensure(ref);
  void* old = Invalidate(ref);
  slot.access_count.store(std::uint16_t{0}, std::memory_order_release);
  return old;
}

void ReplicaDirectory::SetBudgetBytes(std::uint64_t bytes) {
  if (LocalBytes() > bytes) throw std::runtime_error("replica budget below current local usage");
  budget_bytes_.store(bytes, std::memory_order_release);
}

void ReplicaDirectory::RecordAccess(NodeRef ref) const {
  Ensure(ref)->access_count.fetch_add(std::uint16_t{1}, std::memory_order_relaxed);
}

std::uint64_t ReplicaDirectory::AccessCount(NodeRef ref) const {
  if (Slot* slot = Find(ref))
    return slot->access_count.load(std::memory_order_relaxed);
  return 0;
}

void ReplicaDirectory::HalveAccess(NodeRef ref) const {
  Slot* slot = Find(ref);
  if (!slot) return;
  auto value = slot->access_count.load(std::memory_order_relaxed);
  while (!slot->access_count.compare_exchange_weak(value, static_cast<std::uint16_t>(value >> 1), std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

}  // namespace dsidle
