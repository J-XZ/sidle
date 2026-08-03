#include "dsidle/latency_simulator.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
  latency_sim::GlobalLatencySimulator().Configure({});
  alignas(64) std::uint64_t value = 0;
  std::atomic<std::uint64_t> atomic_value{0};
  std::uint64_t checksum = 0;
  constexpr std::uint64_t kIterations = 2'000'000;
  for (std::uint64_t index = 0; index != kIterations; ++index) {
    const auto loaded = latency_sim::CountedMemoryLoad(
        latency_sim::PoolKind::kHwcc, &value);
    latency_sim::CountedMemoryStore(latency_sim::PoolKind::kHwcc, &value,
                                     loaded + 1);
    checksum += latency_sim::CountedAtomicFetchAdd(
        atomic_value, std::uint64_t{1}, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kHwcc);
  }
  assert(value == kIterations);
  assert(atomic_value.load(std::memory_order_relaxed) == kIterations);
  std::cout << "features="
            << latency_sim::HardwareSimulationFeaturesFast()
            << " value=" << value << " checksum=" << checksum << '\n';
}
