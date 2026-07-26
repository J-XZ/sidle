#pragma once

#include "dsidle/shared_pool.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace dsidle {

enum class ReplicaKind : std::uint32_t { kInternal, kValueLeaf, kLayerLeaf };

struct ReplicaSnapshot {
  void* local_ptr{};
  std::uint64_t generation{};
  std::uint64_t cached_version{};
  std::uint64_t bytes{};
  ReplicaKind kind{ReplicaKind::kInternal};
};

// Process-local, slab-indexed replica metadata.  The canonical NodeRef never
// stores a process address; only this local directory does.  Segments are
// allocated lazily in local DRAM, while lock-free readers use a per-slot
// seqlock plus reader count to prevent a publisher/evictor freeing local_ptr
// during a copy window.
class ReplicaDirectory {
 private:
  struct Slot;

 public:
  explicit ReplicaDirectory(const SharedPool& pool);
  ~ReplicaDirectory();
  ReplicaDirectory(const ReplicaDirectory&) = delete;
  ReplicaDirectory& operator=(const ReplicaDirectory&) = delete;

  class ReadHandle {
   public:
    ReadHandle() = default;
    ReadHandle(const ReadHandle&) = delete;
    ReadHandle& operator=(const ReadHandle&) = delete;
    ReadHandle(ReadHandle&& other) noexcept;
    ReadHandle& operator=(ReadHandle&& other) noexcept;
    ~ReadHandle();
    explicit operator bool() const { return snapshot_.local_ptr != nullptr; }
    const ReplicaSnapshot& snapshot() const { return snapshot_; }

   private:
    friend class ReplicaDirectory;
    ReadHandle(Slot* slot, ReplicaSnapshot snapshot) : slot_(slot), snapshot_(snapshot) {}
    void Reset();
    Slot* slot_{};
    ReplicaSnapshot snapshot_{};
  };

  // Acquires a stable local copy only if its generation/version match the
  // canonical control snapshot. The caller keeps the ReadHandle until copied.
  ReadHandle Acquire(NodeRef ref, std::uint64_t generation, std::uint64_t cached_version);
  // Policy-only residency query. Local placement is independent of whether
  // the currently cached snapshot still matches the canonical node version.
  bool HasLocalPlacement(NodeRef ref, std::uint64_t generation) const;

  // Publishes a fully initialized local buffer and returns the superseded
  // buffer only after all local readers have left. Caller owns/free()s it.
  void* Publish(NodeRef ref, ReplicaSnapshot snapshot);
  // Budgeted publication used by D-SIDLE workers.  The directory is the sole
  // owner of local-replica accounting, so an evict/publish race cannot exceed
  // the per-VM budget.  On failure it leaves snapshot.local_ptr untouched.
  bool TryPublish(NodeRef ref, ReplicaSnapshot snapshot, void** superseded);
  // Replaces a stale snapshot only while the same generation is still
  // selected for local placement. This prevents a foreground writer racing a
  // demoter from resurrecting a replica that SIDLE has already evicted.
  bool TryRefresh(NodeRef ref, ReplicaSnapshot snapshot, bool budgeted,
                  void** superseded);
  // Marks a slot invalid, waits for local readers, and returns its old buffer.
  void* Invalidate(NodeRef ref);
  // Called when a NodeControl slot gets a new generation. This both invalidates
  // any stale local buffer and clears the single-index local hotness counter.
  void* ResetForReuse(NodeRef ref);
  void RecordAccess(NodeRef ref) const;
  std::uint64_t AccessCount(NodeRef ref) const;
  // Cooler action: preserve the original SIDLE `access_time >>= 1` rule on
  // the process-local dense counter instead of touching canonical SWCC data.
  void HalveAccess(NodeRef ref) const;
  void RecordInternalHit() { internal_hits_.fetch_add(1, std::memory_order_relaxed); }
  std::uint64_t InternalHits() const { return internal_hits_.load(std::memory_order_relaxed); }
  void SetBudgetBytes(std::uint64_t bytes);
  std::uint64_t LocalBytes() const { return local_bytes_.load(std::memory_order_acquire); }
  std::uint64_t BudgetBytes() const { return budget_bytes_.load(std::memory_order_acquire); }

 private:
  static constexpr std::uint64_t kSlotsPerSegment = 4096;
  struct Slot {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint32_t> readers{0};
    std::atomic<void*> local_ptr{nullptr};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> cached_version{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint32_t> kind{0};
    std::atomic<bool> desired_local{false};
    // Preserve SIDLE's uint16_t access_time modulo arithmetic while keeping
    // each VM's counter race-free and outside canonical SWCC memory.
    std::atomic<std::uint16_t> access_count{0};
  };
  struct Segment { Slot slots[kSlotsPerSegment]; };

  std::uint64_t Index(NodeRef ref) const;
  Slot* Ensure(NodeRef ref) const;
  Slot* Find(NodeRef ref) const;
  void* PublishLocked(NodeRef ref, ReplicaSnapshot snapshot);
  void* InvalidateLocked(NodeRef ref);
  void* InvalidateOlderLocked(NodeRef ref, std::uint64_t generation,
                              std::uint64_t cached_version,
                              std::uint64_t* removed_bytes);
  static void WaitForReaders(Slot& slot);

  std::uint64_t node_control_offset_{};
  std::uint64_t capacity_{};
  std::uint64_t segment_count_{};
  std::unique_ptr<std::atomic<Segment*>[]> segments_;
  mutable std::mutex segment_mutex_;
  mutable std::mutex budget_mutex_;
  std::atomic<std::uint64_t> local_bytes_{0};
  std::atomic<std::uint64_t> budget_bytes_{UINT64_MAX};
  std::atomic<std::uint64_t> internal_hits_{0};
};

// Replica directories and buffers are process-local. Bind one per worker
// thread before it enters Masstree; no directory address enters the pool.
void ConfigureCurrentReplicaDirectory(ReplicaDirectory& directory);
ReplicaDirectory* CurrentReplicaDirectoryOrNull();

}  // namespace dsidle
