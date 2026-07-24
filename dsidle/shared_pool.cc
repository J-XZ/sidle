#include "dsidle/shared_pool.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dsidle {
namespace {
constexpr std::uint64_t kCacheLine = 64;
constexpr std::uint64_t kDiagnosticBytes = 4096;
constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
[[noreturn]] void Fail(const std::string& operation, const std::string& path) {
  throw std::runtime_error(operation + " " + path + ": " + std::strerror(errno));
}
}  // namespace

namespace {
thread_local void* shared_pool_base = nullptr;
}

void SetSharedPoolBase(void* base) { shared_pool_base = base; }
void* SharedPoolBase() {
  if (!shared_pool_base) throw std::runtime_error("D-SIDLE shared pool is not attached in this thread");
  return shared_pool_base;
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
      (expected_bytes && header.total_bytes != expected_bytes) || header.state.load(std::memory_order_acquire) != 1)
    throw std::runtime_error("incompatible or uninitialized D-SIDLE shared pool");
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
  std::atomic_thread_fence(std::memory_order_release);
  header->state.store(1, std::memory_order_release);
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
  if (status.st_size <= 0) { close(fd); throw std::runtime_error("empty D-SIDLE shared pool"); }
  const auto bytes = static_cast<std::uint64_t>(status.st_size);
  void* base = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) { close(fd); Fail("map", path); }
  try { ValidateHeader(*static_cast<PoolHeader*>(base), expected_bytes); }
  catch (...) { munmap(base, bytes); close(fd); throw; }
  SetSharedPoolBase(base);
  return SharedPool(fd, base, bytes);
}

SharedPool::~SharedPool() { Close(); }
SharedPool::SharedPool(SharedPool&& other) noexcept : fd_(other.fd_), base_(other.base_), bytes_(other.bytes_) {
  other.fd_ = -1; other.base_ = nullptr; other.bytes_ = 0;
}
SharedPool& SharedPool::operator=(SharedPool&& other) noexcept {
  if (this != &other) { Close(); fd_ = other.fd_; base_ = other.base_; bytes_ = other.bytes_; other.fd_ = -1; other.base_ = nullptr; other.bytes_ = 0; }
  return *this;
}
void SharedPool::Close() {
  if (base_ && shared_pool_base == base_) shared_pool_base = nullptr;
  if (base_) munmap(base_, bytes_);
  if (fd_ >= 0) close(fd_);
  base_ = nullptr; fd_ = -1; bytes_ = 0;
}

void InitializePoolMetadata(SharedPool& pool, const PoolInitialization& options) {
  if (!options.vm_count || !options.max_threads_per_vm || !options.node_control_capacity)
    throw std::runtime_error("invalid shared-pool metadata parameters");
  auto* layout = pool.static_layout();
  if (layout->node_control_capacity || layout->shard_count || layout->epoch_slot_count)
    throw std::runtime_error("D-SIDLE shared pool metadata is already initialized");

  const auto nodes_offset = AlignUp(sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout), kCacheLine);
  const auto nodes_bytes = options.node_control_capacity * sizeof(NodeControl);
  const auto shards_offset = nodes_offset + nodes_bytes;
  const auto shard_bytes = static_cast<std::uint64_t>(options.vm_count) * kCacheLine;
  const auto epochs_offset = shards_offset + shard_bytes;
  const auto epoch_count = static_cast<std::uint64_t>(options.vm_count) * options.max_threads_per_vm;
  const auto epochs_bytes = epoch_count * sizeof(EpochSlot);
  const auto diagnostic_offset = epochs_offset + epochs_bytes;
  const auto used = diagnostic_offset + kDiagnosticBytes;
  if (used > pool.header()->hwcc_bytes)
    throw std::runtime_error("HWCC capacity exhausted by D-SIDLE static metadata");

  auto* base = static_cast<std::byte*>(pool.base());
  for (std::uint64_t index = 0; index < options.node_control_capacity; ++index)
    new (base + nodes_offset + index * sizeof(NodeControl)) NodeControl{};
  std::memset(base + shards_offset, 0, static_cast<std::size_t>(shard_bytes));
  for (std::uint64_t index = 0; index < epoch_count; ++index)
    new (base + epochs_offset + index * sizeof(EpochSlot)) EpochSlot{};
  std::memset(base + diagnostic_offset, 0, kDiagnosticBytes);
  layout->node_control_offset = nodes_offset;
  layout->node_control_capacity = options.node_control_capacity;
  for (std::uint64_t index = 0; index < options.node_control_capacity; ++index) {
    auto* control = reinterpret_cast<NodeControl*>(base + nodes_offset + index * sizeof(NodeControl));
    control->canonical_swcc_offset = index + 1 == options.node_control_capacity ? 0 : nodes_offset + (index + 1) * sizeof(NodeControl);
  }
  layout->node_free_head.store(nodes_offset, std::memory_order_relaxed);
  layout->shard_controls_offset = shards_offset;
  layout->shard_count = options.vm_count;
  layout->epoch_slots_offset = epochs_offset;
  layout->epoch_slot_count = epoch_count;
  layout->diagnostic_offset = diagnostic_offset;
  std::atomic_thread_fence(std::memory_order_release);
  if (msync(base, static_cast<std::size_t>(used), MS_SYNC) != 0)
    Fail("sync initialized metadata", "shared pool");
}

NodeRef NodeControlSlab::Acquire(std::uint64_t canonical_swcc_offset, std::uint32_t node_type) {
  if (!canonical_swcc_offset) throw std::runtime_error("cannot publish a null canonical node offset");
  auto* metadata = pool_.static_layout();
  auto head = metadata->node_free_head.load(std::memory_order_acquire);
  while (head) {
    auto* control = reinterpret_cast<NodeControl*>(static_cast<std::byte*>(pool_.base()) + head);
    const auto next = control->canonical_swcc_offset;
    if (!metadata->node_free_head.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                                         std::memory_order_acquire))
      continue;
    control->allocation_state = NodeAllocationState::kAllocating;
    control->canonical_swcc_offset = canonical_swcc_offset;
    ++control->generation;
    control->retire_epoch = 0;
    control->node_type = node_type;
    std::atomic_thread_fence(std::memory_order_release);
    control->version_and_state.store(0, std::memory_order_release);
    control->allocation_state = NodeAllocationState::kPublished;
    return NodeRef(head);
  }
  throw std::runtime_error("D-SIDLE NodeControl slab OOM");
}

void NodeControlSlab::Release(NodeRef ref) {
  if (!ref) throw std::runtime_error("cannot release null NodeControl");
  auto* metadata = pool_.static_layout();
  auto* control = ref.get(pool_.base());
  if (!control || control->allocation_state != NodeAllocationState::kRetiring)
    throw std::runtime_error("NodeControl must be RETIRING before release");
  auto head = metadata->node_free_head.load(std::memory_order_acquire);
  do {
    control->canonical_swcc_offset = head;
    control->node_type = 0;
    control->retire_epoch = 0;
    control->allocation_state = NodeAllocationState::kFree;
    std::atomic_thread_fence(std::memory_order_release);
  } while (!metadata->node_free_head.compare_exchange_weak(head, ref.value(), std::memory_order_release,
                                                            std::memory_order_acquire));
}

SharedEpochTable SharedEpochSlots(SharedPool& pool) {
  const auto* layout = pool.static_layout();
  if (!layout->epoch_slots_offset || !layout->shard_count || !layout->epoch_slot_count ||
      layout->epoch_slot_count % layout->shard_count)
    throw std::runtime_error("D-SIDLE epoch slots are not initialized");
  return SharedEpochTable(pool.base(), layout->epoch_slots_offset,
                          static_cast<std::uint32_t>(layout->shard_count),
                          static_cast<std::uint32_t>(layout->epoch_slot_count / layout->shard_count));
}

std::string DescribeHwccBudget(const SharedPool& pool) {
  const auto* layout = pool.static_layout();
  const auto total = layout->diagnostic_offset ? layout->diagnostic_offset + kDiagnosticBytes : 0;
  return "HWCC budget: header/root/static=" + std::to_string(sizeof(PoolHeader) + sizeof(RootControl) + sizeof(PoolStaticLayout)) +
      " node_controls=" + std::to_string(layout->node_control_capacity * sizeof(NodeControl)) +
      " shards=" + std::to_string(layout->shard_count * kCacheLine) +
      " epoch_slots=" + std::to_string(layout->epoch_slot_count * sizeof(EpochSlot)) +
      " diagnostics=" + std::to_string(kDiagnosticBytes) + " total=" + std::to_string(total) +
      " capacity=" + std::to_string(pool.header()->hwcc_bytes);
}

}  // namespace dsidle
