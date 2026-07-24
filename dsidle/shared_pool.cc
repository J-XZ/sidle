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
[[noreturn]] void Fail(const std::string& operation, const std::string& path) {
  throw std::runtime_error(operation + " " + path + ": " + std::strerror(errno));
}
}  // namespace

void SharedPool::ValidateLayout(const PoolLayout& layout) {
  if (!layout.total_bytes || layout.hwcc_offset != 0 || !layout.hwcc_bytes || !layout.swcc_bytes ||
      layout.swcc_offset != layout.hwcc_bytes || layout.hwcc_bytes + layout.swcc_bytes != layout.total_bytes ||
      layout.total_bytes < sizeof(PoolHeader) + sizeof(RootControl))
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
  void* base = mmap(nullptr, layout.total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) { close(fd); Fail("map", path); }
  std::memset(base, 0, static_cast<std::size_t>(layout.total_bytes));
  auto* header = new (base) PoolHeader{};
  header->total_bytes = layout.total_bytes;
  header->hwcc_offset = layout.hwcc_offset;
  header->hwcc_bytes = layout.hwcc_bytes;
  header->swcc_offset = layout.swcc_offset;
  header->swcc_bytes = layout.swcc_bytes;
  new (static_cast<std::byte*>(base) + sizeof(PoolHeader)) RootControl{};
  std::atomic_thread_fence(std::memory_order_release);
  header->state.store(1, std::memory_order_release);
  if (msync(base, sizeof(PoolHeader) + sizeof(RootControl), MS_SYNC) != 0) {
    munmap(base, layout.total_bytes); close(fd); Fail("sync", path);
  }
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
  if (base_) munmap(base_, bytes_);
  if (fd_ >= 0) close(fd_);
  base_ = nullptr; fd_ = -1; bytes_ = 0;
}

}  // namespace dsidle
