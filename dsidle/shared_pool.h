#pragma once

#include "dsidle/node_control.h"

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
  static SharedPool Attach(const std::string& path, std::uint64_t expected_bytes);

  void* base() const { return base_; }
  std::uint64_t size() const { return bytes_; }
  PoolHeader* header() const { return static_cast<PoolHeader*>(base_); }
  RootControl* root_control() const { return reinterpret_cast<RootControl*>(static_cast<std::byte*>(base_) + sizeof(PoolHeader)); }
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

}  // namespace dsidle
