#include "dsidle/latency_simulator.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

latency_sim::Config FixedConfig(double swcc_ns, double hwcc_ns) {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.swcc_fixed_ns_per_line = swcc_ns;
  config.fixed_latency.hwcc_fixed_ns_per_line = hwcc_ns;
  config.fixed_latency.delayed_time_stats_enabled = true;
  return config;
}

latency_sim::Config RemoteConfig() {
  latency_sim::Config config;
  auto& remote = config.remote_cache_invalidation;
  remote.enabled = true;
  remote.node_count = 2;
  remote.cache_line_bytes = 64;
  remote.cache_size_bytes_per_node = 128;
  remote.associativity = 1;
  remote.cache_instances_per_node = 1;
  remote.event_log_capacity = 16;
  return config;
}

}  // namespace

int main() {
  alignas(64) std::array<std::byte, 256> value{};

  {
    latency_sim::Config disabled;
    latency_sim::LatencySimulator simulator(disabled);
    simulator.BeginScope(latency_sim::ScopeKind::kForeground);
    simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                          latency_sim::AccessKind::kRead, value.data(), 64);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, value.data(), 64);
    simulator.EndScopeAndDelay();
    const auto stats = simulator.TakeStatsAndReset();
    assert(simulator.feature_mask() == 0);
    assert(stats.TotalDelayedNs() == 0);
    assert(stats.RawLineAccesses(latency_sim::PoolKind::kHwcc) == 0);
  }

  {
    latency_sim::Config invalid;
    invalid.fixed_latency.cache_line_bytes = 0;
    bool rejected = false;
    try {
      latency_sim::LatencySimulator simulator(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);

    invalid = {};
    invalid.remote_cache_invalidation.cache_line_bytes = 0;
    rejected = false;
    try {
      latency_sim::LatencySimulator simulator(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);

    invalid = {};
    invalid.hwcc_access_count.max_tags = 0;
    rejected = false;
    try {
      latency_sim::LatencySimulator simulator(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);

    invalid = {};
    invalid.fixed_latency.swcc_fixed_ns_per_line = -1.0;
    rejected = false;
    try {
      latency_sim::LatencySimulator simulator(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);

    invalid = {};
    invalid.remote_cache_invalidation.replacement_policy = "unknown";
    rejected = false;
    try {
      latency_sim::LatencySimulator simulator(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    assert(rejected);
  }

  {
    auto config = FixedConfig(3, 7);
    latency_sim::LatencySimulator simulator(config);
    simulator.BeginScope(latency_sim::ScopeKind::kForeground);
    simulator.BeginScope(latency_sim::ScopeKind::kForeground);
    simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                          latency_sim::AccessKind::kRead, value.data(), 80);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, value.data(), 1);
    assert(simulator.PendingDelayNsForTest() == 13);
    simulator.EndScopeAndDelay();
    assert(simulator.PendingDelayNsForTest() == 13);
    simulator.EndScopeAndDelay();
    const auto stats = simulator.TakeStatsAndReset();
    assert(stats.swcc_delayed_ns == 6);
    assert(stats.hwcc_delayed_ns == 7);
    assert(stats.RawLineAccesses(latency_sim::PoolKind::kHwcc) == 0);
  }

  {
    latency_sim::Config config;
    config.hwcc_access_count.enabled = true;
    config.hwcc_access_count.byte_count_enabled = true;
    config.hwcc_access_count.breakdown_by_scope_enabled = true;
    config.hwcc_access_count.breakdown_by_tag_enabled = true;
    latency_sim::LatencySimulator simulator(config);
    simulator.BeginScope(latency_sim::ScopeKind::kForeground);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, value.data(), 80, 2);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kWrite, value.data(), 80, 2);
    // Atomic accesses have their own module and are not plain read/write
    // observations, even when they touch HWCC.
    simulator.RecordAtomicCompleted(
        latency_sim::AtomicDomain::kHwcc,
        latency_sim::AccessKind::kAtomicLoad,
        latency_sim::AtomicOperation::kLoad, value.data(), sizeof(uint64_t),
        false, std::memory_order_acquire, 2);
    simulator.EndScopeAndDelay();
    const auto stats = simulator.TakeStatsAndReset();
    assert(stats.hwcc_read_ops == 1 && stats.hwcc_write_ops == 1);
    assert(stats.hwcc_read_lines == 2 && stats.hwcc_write_lines == 2);
    assert(stats.hwcc_read_bytes == 80 && stats.hwcc_write_bytes == 80);
    assert(stats.hwcc_atomic_ops == 0);
    assert(stats.hwcc_read_ops_by_scope[static_cast<size_t>(
               latency_sim::ScopeKind::kForeground)] == 1);
    assert(stats.hwcc_write_ops_by_tag[2] == 1);
  }

  {
    latency_sim::Config config;
    config.atomic_count.enabled = true;
    config.atomic_count.memory_order_breakdown_enabled = true;
    config.atomic_count.scope_breakdown_enabled = true;
    config.atomic_count.tag_breakdown_enabled = true;
    latency_sim::LatencySimulator simulator(config);
    std::atomic<uint64_t> atomic_value{7};
    simulator.BeginScope(latency_sim::ScopeKind::kForeground);
    const auto loaded = atomic_value.load(std::memory_order_acquire);
    simulator.RecordAtomicCompleted(
        latency_sim::AtomicDomain::kHwcc,
        latency_sim::AccessKind::kAtomicLoad,
        latency_sim::AtomicOperation::kLoad, &atomic_value, sizeof(atomic_value),
        false, std::memory_order_acquire, 3);
    (void)loaded;
    uint64_t expected = 7;
    const bool success = atomic_value.compare_exchange_strong(
        expected, 9, std::memory_order_acq_rel, std::memory_order_acquire);
    simulator.RecordAtomicCompleted(
        latency_sim::AtomicDomain::kHwcc,
        latency_sim::AccessKind::kAtomicRmw,
        latency_sim::AtomicOperation::kCasStrong, &atomic_value,
        sizeof(atomic_value), success, success ? std::memory_order_acq_rel
                                                : std::memory_order_acquire,
        3);
    expected = 7;
    const bool failed = atomic_value.compare_exchange_weak(
        expected, 10, std::memory_order_relaxed, std::memory_order_relaxed);
    simulator.RecordAtomicCompleted(
        latency_sim::AtomicDomain::kHwcc,
        latency_sim::AccessKind::kAtomicRmw,
        latency_sim::AtomicOperation::kCasWeak, &atomic_value,
        sizeof(atomic_value), failed, std::memory_order_relaxed, 3);
    simulator.RecordAtomicCompleted(
        latency_sim::AtomicDomain::kLocalDram,
        latency_sim::AccessKind::kAtomicRmw,
        latency_sim::AtomicOperation::kFetchAdd, &atomic_value,
        sizeof(atomic_value), false, std::memory_order_relaxed, 3);
    simulator.EndScopeAndDelay();
    const auto stats = simulator.TakeStatsAndReset();
    assert(stats.hwcc_atomic_ops == 3);
    assert(stats.hwcc_atomic_loads == 1);
    assert(stats.hwcc_cas_attempts == 2);
    assert(stats.hwcc_cas_successes == (success ? 1u : 0u));
    assert(stats.hwcc_cas_failures == (failed ? 0u : 1u));
    assert(stats.local_dram_atomic_ops == 1);
    assert(stats.atomic_ops_by_domain_tag[3] == 3);
    assert(stats.atomic_ops_by_domain_tag[2 * latency_sim::kMaxBreakdownTags +
                                           3] == 1);
  }

  {
    auto config = RemoteConfig();
    latency_sim::LatencySimulator simulator(config);
    alignas(64) std::atomic<uint64_t> sequence{0};
    alignas(64) std::array<latency_sim::RemoteEventRecord, 16> log{};
    simulator.AttachSharedRemoteLog(&sequence, log.data(), log.size());
    alignas(64) std::array<std::byte, 128> hwcc{};
    alignas(64) std::array<std::byte, 128> swcc{};
    simulator.RegisterMemoryRange(hwcc.data(), hwcc.size(),
                                   latency_sim::MemoryDomain::kHwcc, 7, 0);
    simulator.RegisterMemoryRange(swcc.data(), swcc.size(),
                                   latency_sim::MemoryDomain::kSwcc, 8, 0);
    simulator.SetNodeId(0);
    simulator.RecordRemoteWrite({latency_sim::MemoryDomain::kHwcc, 7, 0}, 0,
                                 false);
    simulator.SetNodeId(1);
    simulator.RecordRemoteRead({latency_sim::MemoryDomain::kHwcc, 7, 0}, 1);
    simulator.RecordRemoteWrite({latency_sim::MemoryDomain::kHwcc, 7, 0}, 1,
                                 false);
    simulator.RecordSwccExplicitHandoff(
        {latency_sim::MemoryDomain::kSwcc, 8, 0}, 0, 1);
    const auto stats = simulator.TakeStatsAndReset();
    assert(stats.remote_events == 4);
    assert(stats.remote_dirty_handoffs == 1);
    assert(stats.remote_clean_copy_invalidations == 1);
    assert(stats.remote_write_transactions_causing_invalidation == 1);
    assert(stats.remote_swcc_explicit_handoffs == 1);
    // The only SWCC event above is the explicit visibility handoff; ordinary
    // SWCC reads/writes never enter this hardware-coherence log.
    assert(stats.remote_dirty_handoffs == 1);
    assert(sequence.load(std::memory_order_acquire) == 4);
    simulator.DetachSharedRemoteLog(&sequence, log.data());
  }

  {
    latency_sim::Config config;
    config.atomic_count.enabled = true;
    config.atomic_count.local_dram_enabled = true;
    latency_sim::GlobalLatencySimulator().Configure(config);
    std::atomic<uint64_t> value_atomic{0};
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      assert(latency_sim::CountedAtomicFetchAdd(
                 value_atomic, uint64_t{1}, std::memory_order_relaxed,
                 latency_sim::AtomicDomain::kLocalDram) == 0);
    }
    std::ostringstream output;
    latency_sim::PrintAndResetLatencySimulatorStats(output, "unit");
    const auto line = output.str();
    assert(line.find("DSIDLE_HARDWARE_SIM_STATS tag=unit") !=
           std::string::npos);
    assert(line.find("local_dram_atomic_ops=1") != std::string::npos);
    latency_sim::GlobalLatencySimulator().Configure({});
  }

  if (latency_sim::TscSpinAvailableForTest()) {
    latency_sim::DelaySpinNsForTest(1);
    std::uint64_t best_ns = std::numeric_limits<std::uint64_t>::max();
    for (int index = 0; index < 50; ++index) {
      const auto begin = std::chrono::steady_clock::now();
      latency_sim::DelaySpinNsForTest(1000);
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - begin)
                               .count();
      best_ns = std::min(best_ns, static_cast<std::uint64_t>(elapsed));
    }
    assert(best_ns >= 700 && best_ns <= 1400);
  }
}
