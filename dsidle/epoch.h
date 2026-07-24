#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace dsidle {
constexpr std::uint64_t kEpochInactive = std::numeric_limits<std::uint64_t>::max();
struct alignas(64) EpochSlot { std::atomic<std::uint64_t> value{kEpochInactive}; std::byte padding[56]{}; };
static_assert(sizeof(EpochSlot) == 64);
class EpochTable {
 public:
  EpochTable(std::uint32_t vms, std::uint32_t threads) : slots_(vms * threads), threads_(threads) {}
  void Enter(std::uint32_t vm, std::uint32_t thread, std::uint64_t epoch) { Slot(vm, thread).value.store(epoch, std::memory_order_release); }
  void Leave(std::uint32_t vm, std::uint32_t thread) { Slot(vm, thread).value.store(kEpochInactive, std::memory_order_release); }
  std::uint64_t MinimumActive() const { std::uint64_t min=kEpochInactive; for (const auto& s: slots_) { auto v=s.value.load(std::memory_order_acquire); if (v != kEpochInactive && v < min) min=v; } return min; }
 private:
  EpochSlot& Slot(std::uint32_t vm, std::uint32_t thread) { if(thread>=threads_ || vm>=slots_.size()/threads_) throw std::runtime_error("invalid epoch slot"); return slots_[vm*threads_+thread]; }
  std::vector<EpochSlot> slots_; std::uint32_t threads_;
};
}
