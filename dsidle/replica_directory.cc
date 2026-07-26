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
    segments_[index].store(nullptr, std::memory_order_relaxed);
}

ReplicaDirectory::~ReplicaDirectory() {
  if (current_replica_directory == this) current_replica_directory = nullptr;
  for (std::uint64_t index = 0; index < segment_count_; ++index) {
    Segment* segment = segments_[index].load(std::memory_order_relaxed);
    if (!segment) continue;
    for (Slot& slot : segment->slots) std::free(slot.local_ptr.load(std::memory_order_relaxed));
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
  if (slot_) slot_->readers.fetch_sub(1, std::memory_order_release);
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
  Segment* segment = segments_[index / kSlotsPerSegment].load(std::memory_order_acquire);
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
  while (slot.readers.load(std::memory_order_acquire) != 0) _mm_pause();
}

ReplicaDirectory::ReadHandle ReplicaDirectory::Acquire(NodeRef ref, std::uint64_t generation,
                                                        std::uint64_t cached_version) const {
  Slot* slot = Find(ref);
  if (!slot) return {};
  while (true) {
    slot->readers.fetch_add(1, std::memory_order_acquire);
    const auto first = slot->seq.load(std::memory_order_acquire);
    if (first & 1) { slot->readers.fetch_sub(1, std::memory_order_release); continue; }
    ReplicaSnapshot snapshot{slot->local_ptr.load(std::memory_order_relaxed),
                             slot->generation.load(std::memory_order_relaxed),
                             slot->cached_version.load(std::memory_order_relaxed),
                             slot->bytes.load(std::memory_order_relaxed),
                             static_cast<ReplicaKind>(slot->kind.load(std::memory_order_relaxed))};
    const auto second = slot->seq.load(std::memory_order_acquire);
    if (first == second && !(second & 1) && snapshot.local_ptr &&
        snapshot.generation == generation && snapshot.cached_version == cached_version)
      return ReadHandle(slot, snapshot);
    slot->readers.fetch_sub(1, std::memory_order_release);
    if (first == second && !(second & 1)) return {};
  }
}

void* ReplicaDirectory::PublishLocked(NodeRef ref, ReplicaSnapshot snapshot) {
  if (!snapshot.local_ptr || !snapshot.generation || !snapshot.bytes)
    throw std::runtime_error("invalid ReplicaDirectory publication");
  Slot& slot = *Ensure(ref);
  auto sequence = slot.seq.load(std::memory_order_acquire);
  while (true) {
    if (sequence & 1) { _mm_pause(); sequence = slot.seq.load(std::memory_order_acquire); continue; }
    if (slot.seq.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) break;
  }
  WaitForReaders(slot);
  void* old = slot.local_ptr.load(std::memory_order_relaxed);
  slot.generation.store(snapshot.generation, std::memory_order_relaxed);
  slot.cached_version.store(snapshot.cached_version, std::memory_order_relaxed);
  slot.bytes.store(snapshot.bytes, std::memory_order_relaxed);
  slot.kind.store(static_cast<std::uint32_t>(snapshot.kind), std::memory_order_relaxed);
  slot.local_ptr.store(snapshot.local_ptr, std::memory_order_relaxed);
  slot.seq.store(sequence + 2, std::memory_order_release);
  return old;
}

void* ReplicaDirectory::Publish(NodeRef ref, ReplicaSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  const auto old_bytes = prior ? prior->bytes.load(std::memory_order_relaxed) : 0;
  void* old = PublishLocked(ref, snapshot);
  local_bytes_.fetch_add(snapshot.bytes - old_bytes, std::memory_order_release);
  return old;
}

bool ReplicaDirectory::TryPublish(NodeRef ref, ReplicaSnapshot snapshot, void** superseded) {
  if (!superseded) throw std::runtime_error("null ReplicaDirectory superseded output");
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* prior = Find(ref);
  const auto old_bytes = prior ? prior->bytes.load(std::memory_order_relaxed) : 0;
  const auto current = local_bytes_.load(std::memory_order_relaxed);
  const auto budget = budget_bytes_.load(std::memory_order_relaxed);
  if (snapshot.bytes > budget || current - old_bytes > budget - snapshot.bytes) return false;
  *superseded = PublishLocked(ref, snapshot);
  local_bytes_.store(current - old_bytes + snapshot.bytes, std::memory_order_release);
  return true;
}

void* ReplicaDirectory::InvalidateLocked(NodeRef ref) {
  Slot* slot = Find(ref);
  if (!slot) return nullptr;
  auto sequence = slot->seq.load(std::memory_order_acquire);
  while (true) {
    if (sequence & 1) { _mm_pause(); sequence = slot->seq.load(std::memory_order_acquire); continue; }
    if (slot->seq.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) break;
  }
  WaitForReaders(*slot);
  void* old = slot->local_ptr.exchange(nullptr, std::memory_order_relaxed);
  slot->generation.store(0, std::memory_order_relaxed);
  slot->cached_version.store(0, std::memory_order_relaxed);
  slot->bytes.store(0, std::memory_order_relaxed);
  slot->kind.store(0, std::memory_order_relaxed);
  slot->seq.store(sequence + 2, std::memory_order_release);
  return old;
}

void* ReplicaDirectory::Invalidate(NodeRef ref) {
  std::lock_guard<std::mutex> lock(budget_mutex_);
  Slot* slot = Find(ref);
  const auto old_bytes = slot ? slot->bytes.load(std::memory_order_relaxed) : 0;
  void* old = InvalidateLocked(ref);
  if (old) local_bytes_.fetch_sub(old_bytes, std::memory_order_release);
  return old;
}

void* ReplicaDirectory::ResetForReuse(NodeRef ref) {
  Slot& slot = *Ensure(ref);
  void* old = Invalidate(ref);
  slot.access_count.store(0, std::memory_order_release);
  return old;
}

void ReplicaDirectory::SetBudgetBytes(std::uint64_t bytes) {
  if (LocalBytes() > bytes) throw std::runtime_error("replica budget below current local usage");
  budget_bytes_.store(bytes, std::memory_order_release);
}

void ReplicaDirectory::RecordAccess(NodeRef ref) const {
  Ensure(ref)->access_count.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t ReplicaDirectory::AccessCount(NodeRef ref) const {
  if (Slot* slot = Find(ref)) return slot->access_count.load(std::memory_order_relaxed);
  return 0;
}

void ReplicaDirectory::HalveAccess(NodeRef ref) const {
  Slot* slot = Find(ref);
  if (!slot) return;
  auto value = slot->access_count.load(std::memory_order_relaxed);
  while (!slot->access_count.compare_exchange_weak(value, value >> 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {}
}

}  // namespace dsidle
