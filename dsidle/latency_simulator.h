#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace latency_sim {
enum class PoolKind : uint8_t { kSwcc = 0, kHwcc = 1 };
enum class AccessKind : uint8_t { kRead, kWrite, kAtomicLoad, kAtomicStore, kAtomicRmw, kFlush };
enum class CacheModel : uint8_t { kNone, kFixedHitRate, kPerThreadLru };
enum class ScopeKind : uint8_t { kForeground, kMerge, kOther };
struct Config {
  bool enabled=false, foreground_enabled=true, merge_enabled=true, stats_enabled=false;
  uint64_t cache_line_bytes=64;
  double swcc_read_ns_per_line=0, swcc_write_ns_per_line=0, swcc_flush_ns_per_line=0;
  double hwcc_read_ns_per_line=0, hwcc_write_ns_per_line=0;
  double hwcc_atomic_load_ns=0, hwcc_atomic_store_ns=0, hwcc_atomic_rmw_ns=0;
  CacheModel cache_model=CacheModel::kNone; bool cache_hits_enabled=true;
  uint64_t cache_capacity_lines=4096, cache_associativity=8;
  double cache_fixed_hit_rate=0, cache_hit_extra_ns=0;
};
struct Stats {
  uint64_t swcc_raw_line_accesses=0, hwcc_raw_line_accesses=0, swcc_cache_hits=0, hwcc_cache_hits=0;
  uint64_t swcc_cache_misses=0, hwcc_cache_misses=0, swcc_delayed_ns=0, hwcc_delayed_ns=0;
  uint64_t RawLineAccesses(PoolKind p) const { return p==PoolKind::kSwcc?swcc_raw_line_accesses:hwcc_raw_line_accesses; }
  uint64_t CacheHits(PoolKind p) const { return p==PoolKind::kSwcc?swcc_cache_hits:hwcc_cache_hits; }
  uint64_t CacheMisses(PoolKind p) const { return p==PoolKind::kSwcc?swcc_cache_misses:hwcc_cache_misses; }
  uint64_t DelayedNs(PoolKind p) const { return p==PoolKind::kSwcc?swcc_delayed_ns:hwcc_delayed_ns; }
};
class LatencySimulator {
 public:
  explicit LatencySimulator(Config config={});
  void Configure(Config config); const Config& config() const { return config_; }
  void BeginScope(ScopeKind); void EndScopeAndDelay();
  void RecordRange(PoolKind, AccessKind, const void*, uint64_t); void RecordLine(PoolKind, AccessKind, const void*);
  Stats SnapshotStats() const; Stats TakeStatsAndReset(); uint64_t PendingDelayNsForTest() const;
 private: Config config_; uint64_t generation_=0; mutable Stats stats_;
};
CacheModel ParseCacheModel(const std::string&); const char* CacheModelName(CacheModel);
LatencySimulator& GlobalLatencySimulator(); bool InstrumentationEnabledFast();
}  // namespace latency_sim
