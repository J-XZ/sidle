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
  ReadHandle Acquire(NodeRef ref, std::uint64_t generation, std::uint64_t cached_version) const;

  // Publishes a fully initialized local buffer and returns the superseded
  // buffer only after all local readers have left. Caller owns/free()s it.
  void* Publish(NodeRef ref, ReplicaSnapshot snapshot);
  // Marks a slot invalid, waits for local readers, and returns its old buffer.
  void* Invalidate(NodeRef ref);

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
  };
  struct Segment { Slot slots[kSlotsPerSegment]; };

  std::uint64_t Index(NodeRef ref) const;
  Slot* Ensure(NodeRef ref);
  Slot* Find(NodeRef ref) const;
  static void WaitForReaders(Slot& slot);

  std::uint64_t node_control_offset_{};
  std::uint64_t capacity_{};
  std::uint64_t segment_count_{};
  std::unique_ptr<std::atomic<Segment*>[]> segments_;
  mutable std::mutex segment_mutex_;
};

}  // namespace dsidle
