#include "dsidle/latency_simulator.h"
#include <cassert>
int main() {
  latency_sim::Config c; c.enabled=true; c.stats_enabled=true; c.swcc_read_ns_per_line=3; c.hwcc_atomic_load_ns=7;
  latency_sim::LatencySimulator s(c); alignas(64) char value[80]{};
  s.BeginScope(latency_sim::ScopeKind::kForeground); s.BeginScope(latency_sim::ScopeKind::kForeground);
  s.RecordRange(latency_sim::PoolKind::kSwcc, latency_sim::AccessKind::kRead, &value, 80);
  s.RecordLine(latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kAtomicLoad, &value);
  assert(s.PendingDelayNsForTest()==13); s.EndScopeAndDelay(); assert(s.PendingDelayNsForTest()==13); s.EndScopeAndDelay();
  const auto st=s.TakeStatsAndReset(); assert(st.swcc_raw_line_accesses==2 && st.swcc_cache_misses==2 && st.swcc_delayed_ns==6); assert(st.hwcc_raw_line_accesses==1 && st.hwcc_cache_misses==1 && st.hwcc_delayed_ns==7);
}
