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
    latency_sim::CountedAtomicStore(
        segments_[index], static_cast<Segment*>(nullptr),
        std::memory_order_relaxed, latency_sim::AtomicDomain::kLocalDram);
}

ReplicaDirectory::~ReplicaDirectory() {
  if (current_replica_directory == this) current_replica_directory = nullptr;
  for (std::uint64_t index = 0; index < segment_count_; ++index) {
    Segment* segment = latency_sim::CountedAtomicLoad(
        segments_[index], std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
    if (!segment) continue;
    for (Slot& slot : segment->slots)
      std::free(latency_sim::CountedAtomicLoad(
          slot.local_ptr, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram));
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
    latency_sim::CountedAtomicFetchSub(
        slot_->readers, std::uint32_t{1}, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
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
  Segment* segment = latency_sim::CountedAtomicLoad(
      segments_[index / kSlotsPerSegment], std::memory_order_acquire,
      latency_sim::AtomicDomain::kLocalDram);
  return segment ? &segment->slots[index % kSlotsPerSegment] : nullptr;
}

ReplicaDirectory::Slot* ReplicaDirectory::Ensure(NodeRef ref) const {
  const auto index = Index(ref);
  auto& published = segments_[index / kSlotsPerSegment];
  Segment* segment = latency_sim::CountedAtomicLoad(
      published, std::memory_order_acquire,
      latency_sim::AtomicDomain::kLocalDram);
  if (!segment) {
    std::lock_guard<std::mutex> lock(segment_mutex_);
    segment = latency_sim::CountedAtomicLoad(
        published, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
    if (!segment) {
      segment = new Segment{};
      latency_sim::CountedAtomicStore(
          published, segment, std::memory_order_release,
          latency_sim::AtomicDomain::kLocalDram);
    }
  }
  return &segment->slots[index % kSlotsPerSegment];
}

void ReplicaDirectory::WaitForReaders(Slot& slot) {
  while (latency_sim::CountedAtomicLoad(
             slot.readers, std::memory_order_acquire,
             latency_sim::AtomicDomain::kLocalDram) != 0)
    _mm_pause();
}

ReplicaDirectory::ReadHandle ReplicaDirectory::Acquire(NodeRef ref, std::uint64_t generation,
                                                        std::uint64_t cached_version) {
  Slot* slot = Find(ref);
  if (!slot) return {};
  while (true) {
    latency_sim::CountedAtomicFetchAdd(
        slot->readers, std::uint32_t{1}, std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
    const auto first = latency_sim::CountedAtomicLoad(
        slot->seq, std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
    if (first & 1) {
      latency_sim::CountedAtomicFetchSub(
          slot->readers, std::uint32_t{1}, std::memory_order_release,
          latency_sim::AtomicDomain::kLocalDram);
      continue;
    }
    ReplicaSnapshot snapshot{
        latency_sim::CountedAtomicLoad(
            slot->local_ptr, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram),
        latency_sim::CountedAtomicLoad(
            slot->generation, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram),
        latency_sim::CountedAtomicLoad(
            slot->cached_version, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram),
        latency_sim::CountedAtomicLoad(
            slot->bytes, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram),
        static_cast<ReplicaKind>(latency_sim::CountedAtomicLoad(
            slot->kind, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram))};
    const auto second = latency_sim::CountedAtomicLoad(
        slot->seq, std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
    if (first == second && !(second & 1) && snapshot.local_ptr &&
        snapshot.generation == generation && snapshot.cached_version == cached_version)
      return ReadHandle(slot, snapshot);
    latency_sim::CountedAtomicFetchSub(
        slot->readers, std::uint32_t{1}, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
    if (first == second && !(second & 1)) {
      std::lock_guard<std::mutex> lock(budget_mutex_);
      std::uint64_t stale_bytes = 0;
      void* stale = InvalidateOlderLocked(
          ref, generation, cached_version, &stale_bytes);
      if (stale) {
        latency_sim::CountedAtomicFetchSub(
            local_bytes_, stale_bytes, std::memory_order_release,
            latency_sim::AtomicDomain::kLocalDram);
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
    const auto first = latency_sim::CountedAtomicLoad(
        slot->seq, std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
    if (first & 1) {
      _mm_pause();
      continue;
    }
    const auto desired =
        latency_sim::CountedAtomicLoad(
            slot->desired_local, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram);
    const auto slot_generation =
        latency_sim::CountedAtomicLoad(
            slot->generation, std::memory_order_relaxed,
            latency_sim::AtomicDomain::kLocalDram);
    const auto second = latency_sim::CountedAtomicLoad(
        slot->seq, std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
    if (first == second)
      return desired && slot_generation == generation;
  }
}

void* ReplicaDirectory::PublishLocked(NodeRef ref, ReplicaSnapshot snapshot) {
  if (!snapshot.local_ptr || !snapshot.generation || !snapshot.bytes)
    throw std::runtime_error("invalid ReplicaDirectory publication");
  Slot& slot = *Ensure(ref);
  auto sequence = latency_sim::CountedAtomicLoad(
      slot.seq, std::memory_order_acquire,
      latency_sim::AtomicDomain::kLocalDram);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = latency_sim::CountedAtomicLoad(
          slot.seq, std::memory_order_acquire,
          latency_sim::AtomicDomain::kLocalDram);
      continue;
    }
    if (latency_sim::CountedCompareExchangeWeak(
            slot.seq, sequence, sequence + 1, std::memory_order_acq_rel,
            std::memory_order_acquire, latency_sim::AtomicDomain::kLocalDram))
      break;
  }
  WaitForReaders(slot);
  void* old = latency_sim::CountedAtomicLoad(
      slot.local_ptr, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.generation, snapshot.generation, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.cached_version, snapshot.cached_version, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.bytes, snapshot.bytes, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.kind, static_cast<std::uint32_t>(snapshot.kind),
      std::memory_order_relaxed, latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.local_ptr, snapshot.local_ptr, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.desired_local, true, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot.seq, sequence + 2, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
  return old;
}

void* ReplicaDirectory::Publish(NodeRef ref, ReplicaSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  auto old_bytes =
      prior ? latency_sim::CountedAtomicLoad(
                   prior->bytes, std::memory_order_relaxed,
                   latency_sim::AtomicDomain::kLocalDram)
            : 0;
  void* old = PublishLocked(ref, snapshot);
  latency_sim::CountedAtomicFetchAdd(
      local_bytes_, snapshot.bytes - old_bytes, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
  return old;
}

bool ReplicaDirectory::TryPublish(NodeRef ref, ReplicaSnapshot snapshot, void** superseded) {
  if (!superseded) throw std::runtime_error("null ReplicaDirectory superseded output");
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  auto old_bytes =
      prior ? latency_sim::CountedAtomicLoad(
                   prior->bytes, std::memory_order_relaxed,
                   latency_sim::AtomicDomain::kLocalDram)
            : 0;
  auto current = latency_sim::CountedAtomicLoad(
      local_bytes_, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  const auto budget = latency_sim::CountedAtomicLoad(
      budget_bytes_, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  if (snapshot.bytes > budget)
    return false;
  if (current - old_bytes > budget - snapshot.bytes) {
    ReclaimRetiredLocked();
    current = latency_sim::CountedAtomicLoad(
        local_bytes_, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
    prior = Find(ref);
    old_bytes =
        prior ? latency_sim::CountedAtomicLoad(
                     prior->bytes, std::memory_order_relaxed,
                     latency_sim::AtomicDomain::kLocalDram)
              : 0;
    if (current - old_bytes > budget - snapshot.bytes)
      return false;
  }
  *superseded = PublishLocked(ref, snapshot);
  latency_sim::CountedAtomicStore(
      local_bytes_, current - old_bytes + snapshot.bytes,
      std::memory_order_release, latency_sim::AtomicDomain::kLocalDram);
  return true;
}

bool ReplicaDirectory::TryRefresh(NodeRef ref, ReplicaSnapshot snapshot,
                                  bool budgeted, void** superseded) {
  if (!superseded)
    throw std::runtime_error("null ReplicaDirectory superseded output");
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  if (!prior ||
      !latency_sim::CountedAtomicLoad(
          prior->desired_local, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram) ||
      latency_sim::CountedAtomicLoad(
          prior->generation, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram) !=
          snapshot.generation)
    return false;
  const auto old_bytes =
      latency_sim::CountedAtomicLoad(
          prior->bytes, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram);
  auto current = latency_sim::CountedAtomicLoad(
      local_bytes_, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  const auto budget = latency_sim::CountedAtomicLoad(
      budget_bytes_, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  if (budgeted) {
    if (snapshot.bytes > budget)
      return false;
    if (current - old_bytes > budget - snapshot.bytes) {
      ReclaimRetiredLocked();
      current = latency_sim::CountedAtomicLoad(
          local_bytes_, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram);
      if (current - old_bytes > budget - snapshot.bytes)
        return false;
    }
  }
  *superseded = PublishLocked(ref, snapshot);
  latency_sim::CountedAtomicStore(
      local_bytes_, current - old_bytes + snapshot.bytes,
      std::memory_order_release, latency_sim::AtomicDomain::kLocalDram);
  return true;
}

void* ReplicaDirectory::InvalidateLocked(NodeRef ref) {
  Slot* slot = Find(ref);
  if (!slot) return nullptr;
  auto sequence = latency_sim::CountedAtomicLoad(
      slot->seq, std::memory_order_acquire,
      latency_sim::AtomicDomain::kLocalDram);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = latency_sim::CountedAtomicLoad(
          slot->seq, std::memory_order_acquire,
          latency_sim::AtomicDomain::kLocalDram);
      continue;
    }
    if (latency_sim::CountedCompareExchangeWeak(
            slot->seq, sequence, sequence + 1, std::memory_order_acq_rel,
            std::memory_order_acquire, latency_sim::AtomicDomain::kLocalDram))
      break;
  }
  WaitForReaders(*slot);
  void* old = latency_sim::CountedAtomicExchange(
      slot->local_ptr, static_cast<void*>(nullptr), std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->cached_version, std::uint64_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->bytes, std::uint64_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->kind, std::uint32_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->generation, std::uint64_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->desired_local, false, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->seq, sequence + 2, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
  return old;
}

void ReplicaDirectory::ReclaimRetiredLocked() {
  for (std::uint64_t segment_index = 0;
       segment_index < segment_count_; ++segment_index) {
    Segment* segment = latency_sim::CountedAtomicLoad(
        segments_[segment_index], std::memory_order_acquire,
        latency_sim::AtomicDomain::kLocalDram);
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
          latency_sim::CountedAtomicLoad(
              slot.generation, std::memory_order_relaxed,
              latency_sim::AtomicDomain::kLocalDram);
      if (!generation &&
          !latency_sim::CountedAtomicLoad(
              slot.local_ptr, std::memory_order_relaxed,
              latency_sim::AtomicDomain::kLocalDram))
        continue;
      const NodeRef ref(
          node_control_offset_ + index * sizeof(NodeControl));
      auto* control = ref.get(SharedPoolBase());
      const auto state = latency_sim::CountedAtomicLoad(
          control->allocation_state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
      const auto control_generation = latency_sim::CountedMemoryLoad(
          latency_sim::PoolKind::kHwcc, &control->generation);
      if (state == NodeAllocationState::kPublished &&
          control_generation == generation)
        continue;
      const auto bytes = latency_sim::CountedAtomicLoad(
          slot.bytes, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kLocalDram);
      void* stale = InvalidateLocked(ref);
      if (stale) {
        latency_sim::CountedAtomicFetchSub(
            local_bytes_, bytes, std::memory_order_release,
            latency_sim::AtomicDomain::kLocalDram);
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
  auto sequence = latency_sim::CountedAtomicLoad(
      slot->seq, std::memory_order_acquire,
      latency_sim::AtomicDomain::kLocalDram);
  while (true) {
    if (sequence & 1) {
      _mm_pause();
      sequence = latency_sim::CountedAtomicLoad(
          slot->seq, std::memory_order_acquire,
          latency_sim::AtomicDomain::kLocalDram);
      continue;
    }
    if (latency_sim::CountedCompareExchangeWeak(
            slot->seq, sequence, sequence + 1, std::memory_order_acq_rel,
            std::memory_order_acquire, latency_sim::AtomicDomain::kLocalDram))
      break;
  }
  // Lazy pruning is opportunistic: unlike an explicit publisher/evictor it
  // must not stall a foreground miss behind another valid ReadHandle.
  if (latency_sim::CountedAtomicLoad(
          slot->readers, std::memory_order_acquire,
          latency_sim::AtomicDomain::kLocalDram) != 0) {
    latency_sim::CountedAtomicStore(
        slot->seq, sequence + 2, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
    return nullptr;
  }
  auto* control = ref.get(SharedPoolBase());
  const auto state = control ? LoadNodeAllocationState(ref)
                             : NodeAllocationState::kFree;
  const auto canonical_version = control
      ? latency_sim::CountedAtomicLoad(
            control->version_and_state, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc)
      : 0;
  if (!control ||
      (state != NodeAllocationState::kPublished &&
       state != NodeAllocationState::kRetiring) ||
      LoadNodeGeneration(ref) != generation ||
      canonical_version != cached_version) {
    latency_sim::CountedAtomicStore(
        slot->seq, sequence + 2, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
    return nullptr;
  }
  const auto slot_generation = latency_sim::CountedAtomicLoad(
      slot->generation, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  const auto slot_version = latency_sim::CountedAtomicLoad(
      slot->cached_version, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  if (slot_generation == generation && slot_version == cached_version) {
    latency_sim::CountedAtomicStore(
        slot->seq, sequence + 2, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
    return nullptr;
  }
  *removed_bytes = latency_sim::CountedAtomicLoad(
      slot->bytes, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  void* stale = latency_sim::CountedAtomicExchange(
      slot->local_ptr, static_cast<void*>(nullptr), std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->cached_version, std::uint64_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->bytes, std::uint64_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  latency_sim::CountedAtomicStore(
      slot->kind, std::uint32_t{0}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  if (slot_generation != generation) {
    latency_sim::CountedAtomicStore(
        slot->generation, std::uint64_t{0}, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
    latency_sim::CountedAtomicStore(
        slot->desired_local, false, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
  }
  latency_sim::CountedAtomicStore(
      slot->seq, sequence + 2, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
  return stale;
}

void* ReplicaDirectory::Invalidate(NodeRef ref) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* slot = Find(ref);
  const auto old_bytes = slot ? latency_sim::CountedAtomicLoad(
                                     slot->bytes, std::memory_order_relaxed,
                                     latency_sim::AtomicDomain::kLocalDram)
                              : 0;
  void* old = InvalidateLocked(ref);
  if (old)
    latency_sim::CountedAtomicFetchSub(
        local_bytes_, old_bytes, std::memory_order_release,
        latency_sim::AtomicDomain::kLocalDram);
  return old;
}

void* ReplicaDirectory::ResetForReuse(NodeRef ref) {
  Slot& slot = *Ensure(ref);
  void* old = Invalidate(ref);
  latency_sim::CountedAtomicStore(
      slot.access_count, std::uint16_t{0}, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
  return old;
}

void ReplicaDirectory::SetBudgetBytes(std::uint64_t bytes) {
  if (LocalBytes() > bytes) throw std::runtime_error("replica budget below current local usage");
  latency_sim::CountedAtomicStore(
      budget_bytes_, bytes, std::memory_order_release,
      latency_sim::AtomicDomain::kLocalDram);
}

void ReplicaDirectory::RecordAccess(NodeRef ref) const {
  latency_sim::CountedAtomicFetchAdd(
      Ensure(ref)->access_count, std::uint16_t{1}, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
}

std::uint64_t ReplicaDirectory::AccessCount(NodeRef ref) const {
  if (Slot* slot = Find(ref))
    return latency_sim::CountedAtomicLoad(
        slot->access_count, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kLocalDram);
  return 0;
}

void ReplicaDirectory::HalveAccess(NodeRef ref) const {
  Slot* slot = Find(ref);
  if (!slot) return;
  auto value = latency_sim::CountedAtomicLoad(
      slot->access_count, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram);
  while (!latency_sim::CountedCompareExchangeWeak(
      slot->access_count, value, static_cast<std::uint16_t>(value >> 1),
      std::memory_order_relaxed, std::memory_order_relaxed,
      latency_sim::AtomicDomain::kLocalDram)) {}
}

}  // namespace dsidle
