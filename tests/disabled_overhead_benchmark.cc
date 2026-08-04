#include "dsidle/latency_simulator.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <vector>

namespace {

double Run(bool wrapped) {
  alignas(64) std::uint64_t value = 0;
  std::atomic<std::uint64_t> atomic_value{0};
  std::uint64_t checksum = 0;
  constexpr std::uint64_t kIterations = 2'000'000;
  const auto begin = std::chrono::steady_clock::now();
  for (std::uint64_t index = 0; index != kIterations; ++index) {
    if (wrapped) {
      const auto loaded = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &value);
      latency_sim::FixedLatencyMemoryStore(latency_sim::PoolKind::kHwcc,
                                           &value, loaded + 1);
      checksum += latency_sim::FixedLatencyAtomicFetchAdd(
          atomic_value, std::uint64_t{1}, std::memory_order_relaxed,
          latency_sim::AtomicDomain::kHwcc);
    } else {
      const auto loaded = value;
      value = loaded + 1;
      checksum += atomic_value.fetch_add(std::uint64_t{1},
                                         std::memory_order_relaxed);
    }
  }
  const auto end = std::chrono::steady_clock::now();
  assert(value == kIterations);
  assert(atomic_value.load(std::memory_order_relaxed) == kIterations);
  if (checksum != kIterations * (kIterations - 1) / 2) std::abort();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                 .count()) /
         kIterations;
}

double Median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

}  // namespace

int main() {
  latency_sim::GlobalLatencySimulator().Configure({});
  std::vector<double> direct;
  std::vector<double> wrapped;
  for (int i = 0; i < 5; ++i) {
    direct.push_back(Run(false));
    wrapped.push_back(Run(true));
  }
  const double direct_median = Median(direct);
  const double wrapped_median = Median(wrapped);
  std::printf(
      "features=%u direct_median_ns_op=%.3f wrapped_median_ns_op=%.3f "
      "direct_min_ns_op=%.3f direct_max_ns_op=%.3f wrapped_min_ns_op=%.3f "
      "wrapped_max_ns_op=%.3f\n",
      latency_sim::FixedLatencyFeaturesFast(), direct_median, wrapped_median,
      *std::min_element(direct.begin(), direct.end()),
      *std::max_element(direct.begin(), direct.end()),
      *std::min_element(wrapped.begin(), wrapped.end()),
      *std::max_element(wrapped.begin(), wrapped.end()));
  std::cout << "features="
            << latency_sim::FixedLatencyFeaturesFast()
            << " direct=" << direct_median << " wrapped=" << wrapped_median
            << '\n';
}
