#include "dsidle/latency_simulator.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <sstream>
int main() {
  latency_sim::Config c; c.enabled=true; c.stats_enabled=true; c.swcc_read_ns_per_line=3; c.hwcc_atomic_load_ns=7;
  latency_sim::LatencySimulator s(c); alignas(64) char value[80]{};
  s.BeginScope(latency_sim::ScopeKind::kForeground); s.BeginScope(latency_sim::ScopeKind::kForeground);
  s.RecordRange(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, &value, 80);
  s.RecordLine(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicLoad, &value);
  assert(s.PendingDelayNsForTest()==13); s.EndScopeAndDelay(); assert(s.PendingDelayNsForTest()==13); s.EndScopeAndDelay();
  const auto st=s.TakeStatsAndReset(); assert(st.swcc_raw_line_accesses==2 && st.swcc_cache_misses==2 && st.swcc_delayed_ns==6); assert(st.hwcc_raw_line_accesses==1 && st.hwcc_cache_misses==1 && st.hwcc_delayed_ns==7);

  latency_sim::Config fixed; fixed.enabled=true; fixed.stats_enabled=true;
  fixed.swcc_read_ns_per_line=11; fixed.cache_model=latency_sim::CacheModel::kFixedHitRate;
  fixed.cache_fixed_hit_rate=1.0; fixed.cache_hit_extra_ns=2;
  latency_sim::LatencySimulator fixed_sim(fixed);
  fixed_sim.BeginScope(latency_sim::ScopeKind::kForeground);
  fixed_sim.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, value);
  assert(fixed_sim.PendingDelayNsForTest()==2);
  fixed_sim.EndScopeAndDelay();
  const auto fixed_stats=fixed_sim.TakeStatsAndReset();
  assert(fixed_stats.swcc_raw_line_accesses==1 && fixed_stats.swcc_cache_hits==1 &&
         fixed_stats.swcc_cache_misses==0 && fixed_stats.swcc_delayed_ns==2 &&
         fixed_stats.TotalLineAccesses(latency_sim::PoolKind::kSwcc)==1);

  latency_sim::Config lru; lru.enabled=true; lru.stats_enabled=true;
  lru.swcc_read_ns_per_line=13; lru.cache_model=latency_sim::CacheModel::kPerThreadLru;
  lru.cache_capacity_lines=2; lru.cache_associativity=2; lru.cache_hit_extra_ns=3;
  latency_sim::LatencySimulator lru_sim(lru);
  lru_sim.BeginScope(latency_sim::ScopeKind::kForeground);
  lru_sim.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, value);
  lru_sim.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, value);
  assert(lru_sim.PendingDelayNsForTest()==16);
  lru_sim.EndScopeAndDelay();
  const auto lru_stats=lru_sim.TakeStatsAndReset();
  assert(lru_stats.swcc_raw_line_accesses==2 && lru_stats.swcc_cache_hits==1 &&
         lru_stats.swcc_cache_misses==1 && lru_stats.swcc_delayed_ns==16 &&
         lru_stats.TotalDelayedNs()==16);

  latency_sim::Config global; global.enabled=true; global.stats_enabled=true;
  global.hwcc_atomic_load_ns=1;
  latency_sim::GlobalLatencySimulator().Configure(global);
  { latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    latency_sim::RecordHwccAtomicLoad(value); }
  std::ostringstream output;
  latency_sim::PrintAndResetLatencySimulatorStats(output, "unit");
  const auto line=output.str();
  assert(line.find("LATENCY_SIM_STATS tag=unit") != std::string::npos);
  assert(line.find("hwcc_raw=1") != std::string::npos);
  assert(line.find("delayed_ns=1") != std::string::npos);

  latency_sim::Config swcc_only; swcc_only.enabled=true; swcc_only.stats_enabled=true;
  swcc_only.swcc_read_ns_per_line=5;
  latency_sim::LatencySimulator swcc_sim(swcc_only);
  swcc_sim.BeginScope(latency_sim::ScopeKind::kForeground);
  swcc_sim.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, value);
  swcc_sim.EndScopeAndDelay();
  const auto swcc_stats=swcc_sim.TakeStatsAndReset();
  assert(swcc_stats.swcc_delayed_ns == 5 && swcc_stats.hwcc_delayed_ns == 0);

  latency_sim::Config hwcc_only; hwcc_only.enabled=true; hwcc_only.stats_enabled=true;
  hwcc_only.hwcc_atomic_load_ns=7;
  latency_sim::LatencySimulator hwcc_sim(hwcc_only);
  hwcc_sim.BeginScope(latency_sim::ScopeKind::kForeground);
  hwcc_sim.RecordLine(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicLoad, value);
  hwcc_sim.EndScopeAndDelay();
  const auto hwcc_stats=hwcc_sim.TakeStatsAndReset();
  assert(hwcc_stats.swcc_delayed_ns == 0 && hwcc_stats.hwcc_delayed_ns == 7);

  latency_sim::Config merge_off; merge_off.enabled=true; merge_off.stats_enabled=true;
  merge_off.merge_enabled=false; merge_off.swcc_read_ns_per_line=11;
  latency_sim::LatencySimulator merge_off_sim(merge_off);
  merge_off_sim.BeginScope(latency_sim::ScopeKind::kMerge);
  merge_off_sim.RecordLine(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, value);
  merge_off_sim.EndScopeAndDelay();
  assert(merge_off_sim.TakeStatsAndReset().TotalDelayedNs() == 0);

  if (latency_sim::TscSpinAvailableForTest()) {
    latency_sim::DelaySpinNsForTest(1);  // Warm the calibrated spin path.
    uint64_t best_ns=std::numeric_limits<uint64_t>::max();
    for (int index=0; index<100; ++index) {
      const auto begin=std::chrono::steady_clock::now();
      latency_sim::DelaySpinNsForTest(1000);
      const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
      best_ns=std::min(best_ns, static_cast<uint64_t>(elapsed));
    }
    assert(best_ns >= 800 && best_ns <= 1200);
  }
}
