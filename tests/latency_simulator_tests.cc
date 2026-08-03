#include "dsidle/latency_simulator.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

latency_sim::Config FixedConfig(double swcc_ns, double hwcc_ns) {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.swcc_fixed_ns_per_line = swcc_ns;
  config.fixed_latency.hwcc_fixed_ns_per_line = hwcc_ns;
  return config;
}

void ExpectInvalid(latency_sim::Config config) {
  bool rejected = false;
  try {
    latency_sim::LatencySimulator simulator(config);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}

void TestDisabledFastPath() {
  latency_sim::LatencySimulator simulator({});
  alignas(64) std::array<std::byte, 256> bytes{};
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  simulator.RecordAtomicAccess(latency_sim::AtomicDomain::kHwcc,
                               latency_sim::AccessKind::kAtomicLoad,
                               bytes.data(), sizeof(std::uint64_t));
  simulator.EndScopeAndDelay();
  assert(simulator.feature_mask() == 0);
  assert(simulator.PendingDelayNsForTest() == 0);
}

void TestValidation() {
  auto invalid = FixedConfig(1, 2);
  invalid.fixed_latency.cache_line_bytes = 0;
  ExpectInvalid(invalid);
  invalid = FixedConfig(1, 2);
  invalid.fixed_latency.cache_line_bytes = 3;
  ExpectInvalid(invalid);
  invalid = FixedConfig(-1, 2);
  ExpectInvalid(invalid);
  invalid = FixedConfig(1, std::numeric_limits<double>::quiet_NaN());
  ExpectInvalid(invalid);
}

void TestLineCoverageAndScopes() {
  alignas(64) std::array<std::byte, 256> bytes{};
  latency_sim::LatencySimulator simulator(FixedConfig(3, 7));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 0);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, nullptr, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kWrite, bytes.data() + 1, 64);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data() + 1, 64);
  assert(simulator.PendingDelayNsForTest() == 26);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  assert(simulator.PendingDelayNsForTest() == 33);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 33);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 0);

  simulator.Configure(FixedConfig(5, 11));
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  simulator.RecordRange(latency_sim::PoolKind::kOwnerPrivateSwcc,
                        latency_sim::AccessKind::kWrite, bytes.data(), 128);
  assert(simulator.PendingDelayNsForTest() == 10);
  simulator.EndScopeAndDelay();
}

void TestScopeGatesAndWorkerIsolation() {
  alignas(64) std::array<std::byte, 128> bytes{};
  auto config = FixedConfig(3, 7);
  config.fixed_latency.background_enabled = false;
  latency_sim::LatencySimulator simulator(config);
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, bytes.data(), 64);
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();

  config.fixed_latency.background_enabled = true;
  simulator.Configure(config);
  std::atomic<std::uint64_t> worker_pending{0};
  std::thread worker([&] {
    simulator.BeginScope(latency_sim::ScopeKind::kMerge);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, bytes.data(), 64);
    worker_pending.store(simulator.PendingDelayNsForTest(),
                         std::memory_order_release);
    simulator.EndScopeAndDelay();
  });
  worker.join();
  assert(worker_pending.load(std::memory_order_acquire) == 7);
  assert(simulator.PendingDelayNsForTest() == 0);
}

void TestAtomicSemantics() {
  alignas(64) std::atomic<std::uint64_t> value{7};
  latency_sim::GlobalLatencySimulator().Configure(FixedConfig(3, 7));
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    assert(latency_sim::FixedLatencyAtomicLoad(
               value, std::memory_order_acquire,
               latency_sim::AtomicDomain::kHwcc) == 7);
    assert(latency_sim::FixedLatencyAtomicExchange(
               value, std::uint64_t{9}, std::memory_order_acq_rel,
               latency_sim::AtomicDomain::kHwcc) == 7);
    assert(latency_sim::FixedLatencyAtomicFetchAdd(
               value, std::uint64_t{1}, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kHwcc) == 9);
    std::uint64_t expected = 10;
    assert(latency_sim::FixedLatencyAtomicCompareExchangeStrong(
        value, expected, std::uint64_t{11}, std::memory_order_acq_rel,
        std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
    expected = 10;
    assert(!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
        value, expected, std::uint64_t{12}, std::memory_order_acq_rel,
        std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
    assert(expected == 11);
    assert(latency_sim::FixedLatencyAtomicFetchOr(
               value, std::uint64_t{1}, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kOwnerPrivateSwcc) == 11);
    assert(latency_sim::FixedLatencyAtomicFetchAnd(
               value, ~std::uint64_t{1}, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kOwnerPrivateSwcc) == 11);
    assert(latency_sim::FixedLatencyAtomicFetchXor(
               value, std::uint64_t{1}, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kLocalDram) == 10);
    assert(latency_sim::GlobalLatencySimulator().PendingDelayNsForTest() ==
           7 * 5 + 3 * 2);
  }
  latency_sim::GlobalLatencySimulator().Configure({});
}

void TestTscSpin() {
  if (!latency_sim::TscSpinAvailableForTest()) return;
  const auto begin = std::chrono::steady_clock::now();
  latency_sim::DelaySpinNsForTest(1000);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - begin).count();
  assert(elapsed >= 700);
}

}  // namespace

int main() {
  TestDisabledFastPath();
  TestValidation();
  TestLineCoverageAndScopes();
  TestScopeGatesAndWorkerIsolation();
  TestAtomicSemantics();
  TestTscSpin();
  latency_sim::GlobalLatencySimulator().Configure({});
}
