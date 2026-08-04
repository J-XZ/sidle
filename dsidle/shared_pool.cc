#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"
#include "dsidle/replica_directory.h"

#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dsidle {
namespace {
constexpr std::uint64_t kCacheLine = 64;
constexpr std::uint64_t kCoordinationBytes = 4096;
constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
[[noreturn]] void Fail(const std::string& operation, const std::string& path) {
  throw std::runtime_error(operation + " " + path + ": " + std::strerror(errno));
}
}  // namespace

namespace {
thread_local void* shared_pool_base = nullptr;
struct SwccAllocatorContext {
  SwccRangeCache range{};
  SharedPool* pool{};
  std::uint32_t shard_count{};
  std::uint32_t local_shard{};
};
thread_local SwccAllocatorContext swcc_allocator_context;

// Migrates the current thread's SWCC binding when a SharedPool object moves.
// The mapping address itself does not change, so the cached SWCC range stays
// valid; only the owning object pointer must follow the destination so its
// Close()/destructor clears the binding, shared-pool base TLS and range
// cache.  Moving a SharedPool is only legal with no concurrent users (all
// workers stopped and joined).
void MigrateThreadBinding(SharedPool* from, SharedPool* to) {
  if (swcc_allocator_context.pool == from) {
    swcc_allocator_context.pool = to;
    if (shared_pool_base == from->base()) shared_pool_base = to->base();
  }
}
}

void SetSharedPoolBase(void* base) { shared_pool_base = base; }
void* SharedPoolBase() {
  if (!shared_pool_base) throw std::runtime_error("D-SIDLE shared pool is not attached in this thread");
  return shared_pool_base;
}

void* SharedPoolBaseOrNull() noexcept { return shared_pool_base; }

void ConfigureCurrentSwccAllocator(SharedPool& pool, std::uint32_t shard_count,
                                   std::uint32_t local_shard) {
  // Explicit binding stage.  The three HWCC fields below are real shared
  // reads; when fixed latency is enabled they must be charged inside the
  // caller's short binding scope (missing scope hard fails in the wrapper).
  const auto layout_shard_count = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool.static_layout()->shard_count);
  const auto swcc_offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool.header()->swcc_offset);
  const auto swcc_bytes = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &pool.header()->swcc_bytes);
  if (!shard_count || local_shard >= shard_count ||
      layout_shard_count != shard_count)
    throw std::runtime_error("invalid D-SIDLE SWCC allocator binding");
  if (swcc_bytes == 0 || swcc_offset > pool.size() ||
      swcc_bytes > pool.size() - swcc_offset)
    throw std::runtime_error("invalid D-SIDLE SWCC mapping range");
  const std::uintptr_t base =
      reinterpret_cast<std::uintptr_t>(pool.base());
  const std::uintptr_t begin = base + swcc_offset;
  if (begin < base || swcc_bytes > std::numeric_limits<std::uintptr_t>::max() - begin)
    throw std::runtime_error("D-SIDLE SWCC mapping range overflows");
  const std::uintptr_t end = begin + swcc_bytes;
  SetSharedPoolBase(pool.base());
  swcc_allocator_context = {SwccRangeCache{begin, end, true}, &pool,
                            shard_count, local_shard};
}

const SwccRangeCache& CurrentSwccRangeCache() {
  if (!swcc_allocator_context.range.valid || !swcc_allocator_context.pool)
    throw std::runtime_error(
        "D-SIDLE SWCC classifier has no bound SWCC allocator; refusing to "
        "classify a potential SWCC access as local DRAM");
  return swcc_allocator_context.range;
}

SharedPool& CurrentSharedPool() {
  if (!swcc_allocator_context.pool)
    throw std::runtime_error("D-SIDLE shared pool is not configured in this thread");
  return *swcc_allocator_context.pool;
}

SharedPool* CurrentSharedPoolOrNull() noexcept {
  return swcc_allocator_context.pool;
}

std::uint32_t CurrentSwccShard() {
  if (!swcc_allocator_context.pool)
    throw std::runtime_error("D-SIDLE SWCC allocator is not configured");
  return swcc_allocator_context.local_shard;
}

SwccOffset<std::byte> AllocateCurrentSwcc(std::uint64_t size) {
  const auto& context = swcc_allocator_context;
  if (!context.pool) throw std::runtime_error("D-SIDLE SWCC allocator is not configured");
  return SwccShardAllocator(*context.pool, context.shard_count).Allocate(context.local_shard, size);
}

std::uint32_t CurrentSwccOwner(SwccOffset<std::byte> block, std::uint64_t size) {
  const auto& context = swcc_allocator_context;
  if (!context.pool) throw std::runtime_error("D-SIDLE SWCC allocator is not configured");
  return SwccShardAllocator(*context.pool, context.shard_count).OwnerOf(block, size);
}

void FreeCurrentSwcc(SwccOffset<std::byte> block, std::uint64_t size, std::uint64_t generation) {
  const auto& context = swcc_allocator_context;
  if (!context.pool) throw std::runtime_error("D-SIDLE SWCC allocator is not configured");
  SwccShardAllocator(*context.pool, context.shard_count).Free(context.local_shard, block, size, generation);
}

void FreeCurrentSwccToOwner(std::uint32_t owner_shard, SwccOffset<std::byte> block,
                            std::uint64_t size, std::uint64_t generation) {
  const auto& context = swcc_allocator_context;
  if (!context.pool) throw std::runtime_error("D-SIDLE SWCC allocator is not configured");
  SwccShardAllocator(*context.pool, context.shard_count).Free(owner_shard, block, size, generation);
}

void SharedPool::ValidateLayout(const PoolLayout& layout) {
  if (!layout.total_bytes || layout.hwcc_offset != 0 || !layout.hwcc_bytes || !layout.swcc_bytes ||
      layout.swcc_offset != layout.hwcc_bytes || layout.hwcc_bytes + layout.swcc_bytes != layout.total_bytes ||
      layout.total_bytes < sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout))
    throw std::runtime_error("invalid HWCC/SWCC pool layout");
  if (!std::atomic<std::uint64_t>{}.is_lock_free())
    throw std::runtime_error("64-bit atomics are not lock-free on this platform");
}

void SharedPool::ValidateHeader(const PoolHeader& header, std::uint64_t expected_bytes) {
  const PoolLayout layout{header.total_bytes, header.hwcc_offset, header.hwcc_bytes,
                          header.swcc_offset, header.swcc_bytes};
  ValidateLayout(layout);
  if (header.magic != kPoolMagic || header.abi_version != kPoolAbiVersion ||
      (expected_bytes && header.total_bytes != expected_bytes))
    throw std::runtime_error("incompatible D-SIDLE shared pool");
  if (latency_sim::FixedLatencyAtomicLoad(
          header.state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      static_cast<std::uint64_t>(PoolState::kReady))
    throw std::runtime_error("D-SIDLE shared pool is not READY");
}

SharedPool SharedPool::Create(const std::string& path, const PoolLayout& layout) {
  ValidateLayout(layout);
  const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  if (fd < 0) Fail("create", path);
  if (ftruncate(fd, static_cast<off_t>(layout.total_bytes)) != 0) { close(fd); Fail("resize", path); }
  close(fd);
  return InitializeExisting(path, layout);
}

SharedPool SharedPool::InitializeExisting(const std::string& path, const PoolLayout& layout) {
  ValidateLayout(layout);
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) Fail("open for initialization", path);
  struct stat status {};
  if (fstat(fd, &status) != 0) { close(fd); Fail("stat", path); }
  if (static_cast<std::uint64_t>(status.st_size) != layout.total_bytes) {
    close(fd);
    throw std::runtime_error("shared pool backing size differs from configured layout");
  }
  void* base = mmap(nullptr, layout.total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) { close(fd); Fail("map", path); }
  // The host launcher clears/prefaults the backing before --init-pool.  Only
  // overwrite the fixed metadata here: touching the complete multi-GB SWCC
  // range would turn initialization into an accidental second prefault pass.
  std::memset(base, 0, sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout));
  auto* header = new (base) PoolHeader{};
  header->total_bytes = layout.total_bytes;
  header->hwcc_offset = layout.hwcc_offset;
  header->hwcc_bytes = layout.hwcc_bytes;
  header->swcc_offset = layout.swcc_offset;
  header->swcc_bytes = layout.swcc_bytes;
  new (static_cast<std::byte*>(base) + sizeof(PoolHeader)) RootControl{};
  new (static_cast<std::byte*>(base) + sizeof(PoolHeader) + sizeof(RootControl)) PoolStaticLayout{};
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  latency_sim::FixedLatencyAtomicStore(
      header->state, static_cast<std::uint64_t>(PoolState::kInitializing),
      std::memory_order_release, latency_sim::AtomicDomain::kHwcc);
  if (msync(base, sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout), MS_SYNC) != 0) {
    munmap(base, layout.total_bytes); close(fd); Fail("sync", path);
  }
  SetSharedPoolBase(base);
  return SharedPool(fd, base, layout.total_bytes);
}

SharedPool SharedPool::Attach(const std::string& path, std::uint64_t expected_bytes) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) Fail("open", path);
  struct stat status {};
  if (fstat(fd, &status) != 0) { close(fd); Fail("stat", path); }
  // The host backing file reports its extent through fstat().  A guest maps
  // the same BAR through the UIO character device, whose st_size is defined
  // as zero; use the experiment's explicit BAR size in that case.
  const auto bytes = status.st_size > 0 ? static_cast<std::uint64_t>(status.st_size)
                                        : (S_ISCHR(status.st_mode) ? expected_bytes : 0);
  if (!bytes) { close(fd); throw std::runtime_error("empty D-SIDLE shared pool"); }
  void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) { close(fd); Fail("map", path); }
  try { ValidateHeader(*static_cast<PoolHeader*>(base), expected_bytes); }
  catch (...) { munmap(base, bytes); close(fd); throw; }
  SetSharedPoolBase(base);
  return SharedPool(fd, base, bytes);
}

SharedPool SharedPool::Attach(const std::string& path,
                              const PoolLayout& expected_layout) {
  ValidateLayout(expected_layout);
  auto pool = Attach(path, expected_layout.total_bytes);
  const auto* header = pool.header();
  if (header->hwcc_offset != expected_layout.hwcc_offset ||
      header->hwcc_bytes != expected_layout.hwcc_bytes ||
      header->swcc_offset != expected_layout.swcc_offset ||
      header->swcc_bytes != expected_layout.swcc_bytes) {
    pool.Close();
    throw std::runtime_error(
        "D-SIDLE shared pool layout differs from configured layout");
  }
  return pool;
}

SharedPool SharedPool::AttachAt(const std::string& path, std::uint64_t expected_bytes,
                                void* requested_base) {
  if (!requested_base) throw std::runtime_error("AttachAt requires a non-null mapping base");
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) Fail("open", path);
  struct stat status {};
  if (fstat(fd, &status) != 0) { close(fd); Fail("stat", path); }
  const auto bytes = status.st_size > 0 ? static_cast<std::uint64_t>(status.st_size)
                                        : (S_ISCHR(status.st_mode) ? expected_bytes : 0);
  if (!bytes) { close(fd); throw std::runtime_error("empty D-SIDLE shared pool"); }
  void* base = mmap(requested_base, bytes, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
  if (base == MAP_FAILED) { close(fd); Fail("map at requested base", path); }
  try { ValidateHeader(*static_cast<PoolHeader*>(base), expected_bytes); }
  catch (...) { munmap(base, bytes); close(fd); throw; }
  SetSharedPoolBase(base);
  return SharedPool(fd, base, bytes);
}

SharedPool::~SharedPool() { Close(); }
SharedPool::SharedPool(SharedPool&& other) noexcept
    : fd_(other.fd_), base_(other.base_), bytes_(other.bytes_) {
  MigrateThreadBinding(&other, this);
  other.fd_ = -1;
  other.base_ = nullptr;
  other.bytes_ = 0;
}
SharedPool& SharedPool::operator=(SharedPool&& other) noexcept {
  if (this != &other) {
    // Close the destination's previous mapping first: this clears its TLS
    // binding and range cache, and never touches a binding that points to
    // the source.  The source binding is migrated below.
    Close();
    fd_ = other.fd_;
    base_ = other.base_;
    bytes_ = other.bytes_;
    MigrateThreadBinding(&other, this);
    other.fd_ = -1;
    other.base_ = nullptr;
    other.bytes_ = 0;
  }
  return *this;
}
void SharedPool::Close() {
  if (base_ && shared_pool_base == base_) shared_pool_base = nullptr;
  if (swcc_allocator_context.pool == this) swcc_allocator_context = {};
  if (base_) munmap(base_, bytes_);
  if (fd_ >= 0) close(fd_);
  base_ = nullptr; fd_ = -1; bytes_ = 0;
}

void InitializePoolMetadata(SharedPool& pool, const PoolInitialization& options) {
  if (latency_sim::FixedLatencyAtomicLoad(
          pool.header()->state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      static_cast<std::uint64_t>(PoolState::kInitializing))
    throw std::runtime_error(
        "D-SIDLE pool metadata requires INITIALIZING state");
  if (!options.vm_count || !options.max_threads_per_vm || !options.node_control_capacity)
    throw std::runtime_error("invalid shared-pool metadata parameters");
  auto* layout = pool.static_layout();
  if (layout->node_control_capacity || layout->shard_count || layout->epoch_slot_count)
    throw std::runtime_error("D-SIDLE shared pool metadata is already initialized");

  const auto nodes_offset = AlignUp(sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout), kCacheLine);
  if (options.node_control_capacity >
      std::numeric_limits<std::uint64_t>::max() / sizeof(NodeControl))
    throw std::runtime_error("D-SIDLE node-control metadata size overflow");
  const auto nodes_bytes = options.node_control_capacity * sizeof(NodeControl);
  const auto shards_offset = nodes_offset + nodes_bytes;
  if (static_cast<std::uint64_t>(options.vm_count) >
      std::numeric_limits<std::uint64_t>::max() /
          (kSwccSizeClassCount * kCacheLine))
    throw std::runtime_error("D-SIDLE shard metadata size overflow");
  const auto shard_bytes = static_cast<std::uint64_t>(options.vm_count) * kSwccSizeClassCount * kCacheLine;
  const auto epochs_offset = shards_offset + shard_bytes;
  if (static_cast<std::uint64_t>(options.vm_count) >
      std::numeric_limits<std::uint64_t>::max() / options.max_threads_per_vm ||
      static_cast<std::uint64_t>(options.vm_count) * options.max_threads_per_vm >
          std::numeric_limits<std::uint64_t>::max() / sizeof(EpochSlot))
    throw std::runtime_error("D-SIDLE epoch metadata size overflow");
  const auto epoch_count = static_cast<std::uint64_t>(options.vm_count) * options.max_threads_per_vm;
  const auto epochs_bytes = epoch_count * sizeof(EpochSlot);
  const auto coordination_offset = epochs_offset + epochs_bytes;
  const auto coordination_end =
      coordination_offset + kCoordinationBytes;
  if (coordination_end < coordination_offset ||
      coordination_end > pool.header()->hwcc_bytes)
    throw std::runtime_error(
        "HWCC capacity exhausted by D-SIDLE coordination metadata");
  const auto used = coordination_end;

  auto* base = static_cast<std::byte*>(pool.base());
  for (std::uint64_t index = 0; index < options.node_control_capacity; ++index)
    new (base + nodes_offset + index * sizeof(NodeControl)) NodeControl{};
  std::memset(base + shards_offset, 0, static_cast<std::size_t>(shard_bytes));
  for (std::uint64_t index = 0; index < epoch_count; ++index)
    new (base + epochs_offset + index * sizeof(EpochSlot)) EpochSlot{};
  std::memset(base + coordination_offset, 0, kCoordinationBytes);
  // The epoch clock and phase barrier start at the real business coordination
  // region base; no legacy instrumentation hole is preserved.
  new (base + coordination_offset) SharedEpochClock{};
  auto* phase_barrier =
      new (base + coordination_offset + sizeof(SharedEpochClock))
          SharedPhaseBarrier{};
  phase_barrier->participants = options.vm_count;
  layout->node_control_offset = nodes_offset;
  layout->node_control_capacity = options.node_control_capacity;
  for (std::uint64_t index = 0; index < options.node_control_capacity; ++index) {
    auto* control = reinterpret_cast<NodeControl*>(base + nodes_offset + index * sizeof(NodeControl));
    control->canonical_swcc_offset = index + 1 == options.node_control_capacity ? 0 : nodes_offset + (index + 1) * sizeof(NodeControl);
  }
  latency_sim::FixedLatencyAtomicStore(
      layout->node_free_head, TaggedFreeListHead::Encode(nodes_offset, 0),
      std::memory_order_relaxed, latency_sim::AtomicDomain::kHwcc);
  layout->shard_controls_offset = shards_offset;
  layout->shard_count = options.vm_count;
  layout->epoch_slots_offset = epochs_offset;
  layout->epoch_slot_count = epoch_count;
  layout->coordination_offset = coordination_offset;
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  if (msync(base, static_cast<std::size_t>(used), MS_SYNC) != 0)
    Fail("sync initialized metadata", "shared pool");
}

void FinalizePoolInitialization(SharedPool& pool) {
  auto* header = pool.header();
  if (latency_sim::FixedLatencyAtomicLoad(
          header->state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
      static_cast<std::uint64_t>(PoolState::kInitializing))
    throw std::runtime_error(
        "D-SIDLE pool finalization requires INITIALIZING state");
  const auto* layout = pool.static_layout();
  if (!layout->node_control_offset || !layout->node_control_capacity ||
      !layout->shard_controls_offset || !layout->shard_count ||
      !layout->epoch_slots_offset || !layout->epoch_slot_count ||
      !layout->coordination_offset)
    throw std::runtime_error(
        "D-SIDLE pool metadata is incomplete at finalization");

  const auto* base = static_cast<const std::byte*>(pool.base());
  for (std::uint64_t shard = 0; shard < layout->shard_count; ++shard) {
    for (std::uint32_t index = 0; index < kSwccSizeClassCount; ++index) {
      const auto* control = reinterpret_cast<const ShardControl*>(
          base + layout->shard_controls_offset +
          (shard * kSwccSizeClassCount + index) * sizeof(ShardControl));
      const auto bump = latency_sim::FixedLatencyAtomicLoad(
          control->bump, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc);
      const auto limit = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &control->limit);
      if (!bump || bump >= limit)
        throw std::runtime_error(
            "D-SIDLE SWCC allocator classes are incomplete at finalization");
    }
  }

  // The earlier metadata sync and this HWCC sync complete before READY is
  // released. No process can attach and observe a partially initialized
  // allocator/control layout.
  if (msync(pool.base(), static_cast<std::size_t>(header->hwcc_bytes),
            MS_SYNC) != 0)
    Fail("sync finalized metadata", "shared pool");
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  latency_sim::FixedLatencyAtomicStore(
      header->state, static_cast<std::uint64_t>(PoolState::kReady),
      std::memory_order_release, latency_sim::AtomicDomain::kHwcc);
  if (msync(pool.base(), sizeof(PoolHeader), MS_SYNC) != 0)
    Fail("sync ready state", "shared pool");
}

NodeRef NodeControlSlab::Reserve(std::uint64_t canonical_swcc_offset,
                                std::uint32_t node_type,
                                std::size_t canonical_bytes) {
  if (!canonical_swcc_offset) throw std::runtime_error("cannot reserve a null canonical node offset");
  if (!canonical_bytes || canonical_bytes > kCanonicalNodeEnvelopeBytes ||
      node_type >= (std::uint32_t{1} << kNodeKindBits))
    throw std::runtime_error("invalid canonical node metadata");
  auto* metadata = pool_.static_layout();
  auto head = latency_sim::FixedLatencyAtomicLoad(
      metadata->node_free_head, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  while (const auto offset = TaggedFreeListHead::Offset(head)) {
    auto* control = reinterpret_cast<NodeControl*>(
        static_cast<std::byte*>(pool_.base()) + offset);
    const auto next = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::PoolKind::kHwcc, &control->canonical_swcc_offset);
    if (!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
            metadata->node_free_head, head,
            TaggedFreeListHead::Advance(head, next),
            std::memory_order_acq_rel, std::memory_order_acquire,
            latency_sim::AtomicDomain::kHwcc))
      continue;
    latency_sim::FixedLatencyAtomicStore(
        control->allocation_state, NodeAllocationState::kAllocating,
        std::memory_order_relaxed, latency_sim::AtomicDomain::kHwcc);
    const auto generation = latency_sim::FixedLatencyMemoryLoad(
        latency_sim::PoolKind::kHwcc, &control->generation);
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->canonical_swcc_offset,
                                    canonical_swcc_offset);
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->generation, generation + 1);
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->retire_epoch, std::uint64_t{0});
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::PoolKind::kHwcc, &control->node_type,
        (static_cast<std::uint32_t>(canonical_bytes) << kNodeKindBits) |
            node_type);
    latency_sim::FixedLatencyAtomicStore(
        control->leaf_link_lock, 0U, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->phantom_epoch, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->parent_ref, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->version_and_state, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    if (auto* directory = CurrentReplicaDirectoryOrNull())
      std::free(directory->ResetForReuse(NodeRef(offset)));
    return NodeRef(offset);
  }
  throw std::runtime_error("D-SIDLE NodeControl slab OOM");
}

void NodeControlSlab::Cancel(NodeRef ref) {
  if (!ref) throw std::runtime_error("cannot cancel null NodeControl");
  auto* metadata = pool_.static_layout();
  auto* control = ref.get(pool_.base());
  if (!control ||
      latency_sim::FixedLatencyAtomicLoad(
          control->allocation_state, std::memory_order_acquire,
          latency_sim::AtomicDomain::kHwcc) !=
          NodeAllocationState::kAllocating)
    throw std::runtime_error(
        "NodeControl must be ALLOCATING before cancellation");
  auto head = latency_sim::FixedLatencyAtomicLoad(
      metadata->node_free_head, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  do {
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::PoolKind::kHwcc, &control->canonical_swcc_offset,
        TaggedFreeListHead::Offset(head));
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->node_type, 0U);
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->retire_epoch, std::uint64_t{0});
    latency_sim::FixedLatencyAtomicStore(control->leaf_link_lock, 0U,
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->phantom_epoch, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->parent_ref, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->version_and_state, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->allocation_state,
                                    NodeAllocationState::kFree,
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
  } while (!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      metadata->node_free_head, head,
      TaggedFreeListHead::Advance(head, ref.value()),
      std::memory_order_release, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc));
}

void NodeControlSlab::Publish(NodeRef ref, std::uint64_t initial_version) {
  if (!ref) throw std::runtime_error("cannot publish null NodeControl");
  auto* control = ref.get(pool_.base());
  if (!control || latency_sim::FixedLatencyAtomicLoad(
                       control->allocation_state, std::memory_order_acquire,
                       latency_sim::AtomicDomain::kHwcc) !=
                       NodeAllocationState::kAllocating)
    throw std::runtime_error("NodeControl must be ALLOCATING before publish");
  // The caller has initialized and flushed the canonical SWCC object before
  // this release store makes its control line readable by other VMs.
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  latency_sim::FixedLatencyAtomicStore(control->version_and_state, initial_version,
                                  std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  latency_sim::FixedLatencyAtomicStore(
      control->allocation_state, NodeAllocationState::kPublished,
      std::memory_order_release, latency_sim::AtomicDomain::kHwcc);
}

void NodeControlSlab::Retire(NodeRef ref, std::uint64_t retire_epoch) {
  if (!ref) throw std::runtime_error("cannot retire null NodeControl");
  auto* control = ref.get(pool_.base());
  if (!control || latency_sim::FixedLatencyAtomicLoad(
                       control->allocation_state, std::memory_order_acquire,
                       latency_sim::AtomicDomain::kHwcc) !=
                       NodeAllocationState::kPublished)
    throw std::runtime_error("NodeControl must be PUBLISHED before retire");
  latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                  &control->retire_epoch, retire_epoch);
  latency_sim::FixedLatencyAtomicStore(
      control->allocation_state, NodeAllocationState::kRetiring,
      std::memory_order_release, latency_sim::AtomicDomain::kHwcc);
  latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                  latency_sim::AtomicDomain::kHwcc);
  if (auto* directory = CurrentReplicaDirectoryOrNull())
    std::free(directory->Invalidate(ref));
}

void NodeControlSlab::Release(NodeRef ref) {
  if (!ref) throw std::runtime_error("cannot release null NodeControl");
  auto* metadata = pool_.static_layout();
  auto* control = ref.get(pool_.base());
  if (!control || latency_sim::FixedLatencyAtomicLoad(
                       control->allocation_state, std::memory_order_acquire,
                       latency_sim::AtomicDomain::kHwcc) !=
                       NodeAllocationState::kRetiring)
    throw std::runtime_error("NodeControl must be RETIRING before release");
  auto head = latency_sim::FixedLatencyAtomicLoad(
      metadata->node_free_head, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc);
  do {
    latency_sim::FixedLatencyMemoryStore(
        latency_sim::PoolKind::kHwcc, &control->canonical_swcc_offset,
        TaggedFreeListHead::Offset(head));
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->node_type, 0U);
    latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                    &control->retire_epoch, std::uint64_t{0});
    latency_sim::FixedLatencyAtomicStore(control->leaf_link_lock, 0U,
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->phantom_epoch, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->parent_ref, std::uint64_t{0},
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicStore(control->allocation_state,
                                    NodeAllocationState::kFree,
                                    std::memory_order_relaxed,
                                    latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicFence(std::memory_order_release,
                                    latency_sim::AtomicDomain::kHwcc);
  } while (!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
      metadata->node_free_head, head,
      TaggedFreeListHead::Advance(head, ref.value()),
      std::memory_order_release, std::memory_order_acquire,
      latency_sim::AtomicDomain::kHwcc));
}

SharedEpochTable SharedEpochSlots(SharedPool& pool) {
  const auto* layout = pool.static_layout();
  const auto shard_count = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &layout->shard_count);
  const auto epoch_slots_offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &layout->epoch_slots_offset);
  const auto epoch_slot_count = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &layout->epoch_slot_count);
  if (!epoch_slots_offset || !shard_count || !epoch_slot_count ||
      epoch_slot_count % shard_count)
    throw std::runtime_error("D-SIDLE epoch slots are not initialized");
  return SharedEpochTable(pool.base(), epoch_slots_offset,
                          static_cast<std::uint32_t>(shard_count),
                          static_cast<std::uint32_t>(epoch_slot_count / shard_count));
}

SharedEpochClockView SharedEpochState(SharedPool& pool) {
  const auto* layout = pool.static_layout();
  const auto coordination_offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc, &layout->coordination_offset);
  if (!coordination_offset)
    throw std::runtime_error("D-SIDLE epoch clock is not initialized");
  return SharedEpochClockView(reinterpret_cast<SharedEpochClock*>(
      static_cast<std::byte*>(pool.base()) + coordination_offset));
}

SharedPhaseBarrierView SharedExperimentPhaseBarrier(SharedPool& pool) {
  const auto coordination_offset = latency_sim::FixedLatencyMemoryLoad(
      latency_sim::PoolKind::kHwcc,
      &pool.static_layout()->coordination_offset);
  return SharedPhaseBarrierView(reinterpret_cast<SharedPhaseBarrier*>(
      static_cast<std::byte*>(pool.base()) + coordination_offset +
      sizeof(SharedEpochClock)));
}

void ConfigureLatencySimulatorForPool(
    SharedPool& pool, const latency_sim::Config& config, std::uint32_t node_id) {
  (void)node_id;
  auto& simulator = latency_sim::GlobalLatencySimulator();
  // Explicit startup boundary: disable any previously enabled state and drop
  // stale registrations from an earlier mapping in this process before
  // registering the current immutable HWCC/SWCC ranges.
  simulator.Configure(latency_sim::Config{});
  simulator.ClearPoolRegistrations();
  // Register the immutable HWCC/SWCC mapping boundaries before enabling fixed
  // latency.  Owner-private SWCC validates against the SWCC region.
  simulator.RegisterPool(latency_sim::PoolKind::kHwcc, pool.hwcc_base(),
                         pool.header()->hwcc_bytes);
  simulator.RegisterPool(latency_sim::PoolKind::kSwcc, pool.swcc_base(),
                         pool.header()->swcc_bytes);
  simulator.Configure(config);
}

std::string DescribeHwccBudget(const SharedPool& pool) {
  const auto* layout = pool.static_layout();
  const auto used = layout->coordination_offset
                        ? layout->coordination_offset + kCoordinationBytes
                        : 0;
  return "HWCC budget: header/root/static=" +
      std::to_string(sizeof(PoolHeader) + sizeof(RootControl) +
                     sizeof(PoolStaticLayout)) +
      " node_controls=" +
      std::to_string(layout->node_control_capacity * sizeof(NodeControl)) +
      " shard_classes=" +
      std::to_string(layout->shard_count * kSwccSizeClassCount * kCacheLine) +
      " epoch_slots=" +
      std::to_string(layout->epoch_slot_count * sizeof(EpochSlot)) +
      " coordination=" + std::to_string(kCoordinationBytes) +
      " metadata_end=" + std::to_string(used) +
      " capacity=" + std::to_string(pool.header()->hwcc_bytes);
}

}  // namespace dsidle
