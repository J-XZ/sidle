#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include "dsidle/latency_simulator.h"
namespace dsidle {
constexpr std::uint64_t kEpochInactive = std::numeric_limits<std::uint64_t>::max();
struct alignas(64) EpochSlot { std::atomic<std::uint64_t> value{kEpochInactive}; std::byte padding[56]{}; };
static_assert(sizeof(EpochSlot) == 64);

// The first line of the fixed diagnostic region is the distributed RCU clock.
// It is separate from the per-thread slots, so advancing it never modifies a
// remote VM's foreground state.
struct alignas(64) SharedEpochClock {
  std::atomic<std::uint64_t> value{1};
  std::byte padding[56]{};
};
static_assert(sizeof(SharedEpochClock) == 64);

// A reusable process barrier in the HWCC diagnostic region. It is intentionally
// separate from epoch slots: waiting at an experiment phase boundary must not
// make a VM appear inside an RCU critical section.
struct alignas(64) SharedPhaseBarrier {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<std::uint32_t> arrived{0};
  std::uint32_t participants{0};
  std::byte padding[48]{};
};
static_assert(sizeof(SharedPhaseBarrier) == 64);

class SharedEpochClockView {
 public:
  explicit SharedEpochClockView(SharedEpochClock* clock) : clock_(clock) {}
  std::uint64_t Current() const { latency_sim::RecordHwccAtomicLoad(&clock_->value); return clock_->value.load(std::memory_order_acquire); }
  std::uint64_t Advance() const { latency_sim::RecordHwccAtomicRmw(&clock_->value); return clock_->value.fetch_add(1, std::memory_order_acq_rel) + 1; }
 private:
  SharedEpochClock* clock_;
};

class SharedPhaseBarrierView {
 public:
  explicit SharedPhaseBarrierView(SharedPhaseBarrier* barrier) : barrier_(barrier) {}
  void Wait() const {
    if (!barrier_ || !barrier_->participants) throw std::runtime_error("invalid shared phase barrier");
    const auto generation = barrier_->generation.load(std::memory_order_acquire);
    if (barrier_->arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == barrier_->participants) {
      barrier_->arrived.store(0, std::memory_order_release);
      barrier_->generation.fetch_add(1, std::memory_order_release);
      return;
    }
    while (barrier_->generation.load(std::memory_order_acquire) == generation)
      std::this_thread::yield();
  }
 private:
  SharedPhaseBarrier* barrier_{};
};

class EpochTable {
 public:
  EpochTable(std::uint32_t vms, std::uint32_t threads) : slots_(vms * threads), threads_(threads) {}
  // Process-local reference implementation used by unit tests. Its vector is
  // ordinary DRAM, not the shared pool, so it must not create HWCC traffic.
  void Enter(std::uint32_t vm, std::uint32_t thread, std::uint64_t epoch) { Slot(vm, thread).value.store(epoch, std::memory_order_release); }
  void Leave(std::uint32_t vm, std::uint32_t thread) { Slot(vm, thread).value.store(kEpochInactive, std::memory_order_release); }
  std::uint64_t MinimumActive() const { std::uint64_t min=kEpochInactive; for (const auto& s: slots_) { auto v=s.value.load(std::memory_order_acquire); if (v != kEpochInactive && v < min) min=v; } return min; }
 private:
  EpochSlot& Slot(std::uint32_t vm, std::uint32_t thread) { if(thread>=threads_ || vm>=slots_.size()/threads_) throw std::runtime_error("invalid epoch slot"); return slots_[vm*threads_+thread]; }
  std::vector<EpochSlot> slots_; std::uint32_t threads_;
};

// A pool-resident view used by the distributed runtime; its backing slots are
// allocated in HWCC and therefore survive process attach at different bases.
class SharedEpochTable {
 public:
  SharedEpochTable(void* base, std::uint64_t offset, std::uint32_t vms, std::uint32_t threads)
      : slots_(reinterpret_cast<EpochSlot*>(static_cast<std::byte*>(base) + offset)), vms_(vms), threads_(threads) {}
  void Enter(std::uint32_t vm, std::uint32_t thread, std::uint64_t epoch) { auto& slot=Slot(vm, thread); latency_sim::RecordHwccAtomicStore(&slot.value); slot.value.store(epoch, std::memory_order_release); }
  void Leave(std::uint32_t vm, std::uint32_t thread) { auto& slot=Slot(vm, thread); latency_sim::RecordHwccAtomicStore(&slot.value); slot.value.store(kEpochInactive, std::memory_order_release); }
  std::uint64_t MinimumActive() const { std::uint64_t min=kEpochInactive; for (std::uint32_t i=0;i<vms_*threads_;++i) { latency_sim::RecordHwccAtomicLoad(&slots_[i].value); auto v=slots_[i].value.load(std::memory_order_acquire); if(v!=kEpochInactive && v<min) min=v; } return min; }
 private:
  EpochSlot& Slot(std::uint32_t vm, std::uint32_t thread) { if(vm>=vms_ || thread>=threads_) throw std::runtime_error("invalid epoch slot"); return slots_[vm*threads_+thread]; }
  EpochSlot* slots_; std::uint32_t vms_, threads_;
};
}
