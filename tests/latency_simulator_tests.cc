#include "dsidle/latency_simulator.h"
#include <cassert>
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
}
