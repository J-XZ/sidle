#pragma once

#include "dsidle/node_control.h"
#include "dsidle/epoch.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dsidle {

constexpr std::uint64_t kPoolMagic = 0x445349444c455031ULL;  // "DSIDLEP1"
constexpr std::uint64_t kPoolAbiVersion = 1;

struct PoolLayout {
  std::uint64_t total_bytes{};
  std::uint64_t hwcc_offset{};
  std::uint64_t hwcc_bytes{};
  std::uint64_t swcc_offset{};
  std::uint64_t swcc_bytes{};
};

// The fixed metadata line follows RootControl.  It describes the remainder of
// the prescribed HWCC layout without putting process-local addresses in the
// backing file.
struct alignas(64) PoolStaticLayout {
  std::uint64_t node_control_offset{};
  std::uint64_t node_control_capacity{};
  std::atomic<std::uint64_t> node_free_head{0};
  std::uint64_t shard_controls_offset{};
  std::uint64_t shard_count{};
  std::uint64_t epoch_slots_offset{};
  std::uint64_t epoch_slot_count{};
  std::uint64_t diagnostic_offset{};
};
static_assert(sizeof(PoolStaticLayout) == 64 && alignof(PoolStaticLayout) == 64);

struct PoolInitialization {
  std::uint32_t vm_count{};
  std::uint32_t max_threads_per_vm{};
  std::uint64_t node_control_capacity{2'097'152};
};

// The fixed first cache line is deliberately small: all extensible metadata
// follows it in the prescribed HWCC order in later M1 steps.
struct alignas(64) PoolHeader {
  std::uint64_t magic{kPoolMagic};
  std::uint64_t abi_version{kPoolAbiVersion};
  std::uint64_t total_bytes{};
  std::uint64_t hwcc_offset{};
  std::uint64_t hwcc_bytes{};
  std::uint64_t swcc_offset{};
  std::uint64_t swcc_bytes{};
  std::atomic<std::uint64_t> state{0};
};
static_assert(sizeof(PoolHeader) == 64 && alignof(PoolHeader) == 64);

class SharedPool {
 public:
  SharedPool() = default;
  ~SharedPool();
  SharedPool(const SharedPool&) = delete;
  SharedPool& operator=(const SharedPool&) = delete;
  SharedPool(SharedPool&& other) noexcept;
  SharedPool& operator=(SharedPool&& other) noexcept;

  static SharedPool Create(const std::string& path, const PoolLayout& layout);
  // Replaces the contents of an already-created backing file during explicit
  // --init-pool setup. It never resizes the file.
  static SharedPool InitializeExisting(const std::string& path, const PoolLayout& layout);
  static SharedPool Attach(const std::string& path, std::uint64_t expected_bytes);

  void* base() const { return base_; }
  std::uint64_t size() const { return bytes_; }
  PoolHeader* header() const { return static_cast<PoolHeader*>(base_); }
  RootControl* root_control() const { return reinterpret_cast<RootControl*>(static_cast<std::byte*>(base_) + sizeof(PoolHeader)); }
  PoolStaticLayout* static_layout() const { return reinterpret_cast<PoolStaticLayout*>(static_cast<std::byte*>(base_) + sizeof(PoolHeader) + sizeof(RootControl)); }
  void* hwcc_base() const { return static_cast<std::byte*>(base_) + header()->hwcc_offset; }
  void* swcc_base() const { return static_cast<std::byte*>(base_) + header()->swcc_offset; }
  void Close();

 private:
  SharedPool(int fd, void* base, std::uint64_t bytes) : fd_(fd), base_(base), bytes_(bytes) {}
  static void ValidateLayout(const PoolLayout& layout);
  static void ValidateHeader(const PoolHeader& header, std::uint64_t expected_bytes);

  int fd_{-1};
  void* base_{nullptr};
  std::uint64_t bytes_{0};
};

// Initializes all fixed HWCC structures after the backing file was created or
// prefaulted by the host launcher.  This is intentionally a one-shot command.
void InitializePoolMetadata(SharedPool& pool, const PoolInitialization& options);
SharedEpochTable SharedEpochSlots(SharedPool& pool);
std::string DescribeHwccBudget(const SharedPool& pool);

// The Masstree runtime binds one process-local shard before creating its
// threadinfos. Persistent data allocation is then always in SWCC; no DAX or
// memkind fallback exists.
void ConfigureCurrentSwccAllocator(SharedPool& pool, std::uint32_t shard_count,
                                   std::uint32_t local_shard);
SwccOffset<std::byte> AllocateCurrentSwcc(std::uint64_t size);
void FreeCurrentSwcc(SwccOffset<std::byte> block, std::uint64_t size,
                     std::uint64_t generation = 0);

}  // namespace dsidle
