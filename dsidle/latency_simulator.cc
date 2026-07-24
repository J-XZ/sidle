#include "dsidle/latency_simulator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace latency_sim {
namespace {
std::atomic<bool> g_instrumentation_enabled{false};
std::atomic<uint64_t> g_simulator_generation{0};
std::once_flag g_tsc_calibration_once;
std::atomic<double> g_tsc_ticks_per_ns{0.0};

uint64_t NextGeneration() {
  const uint64_t value = g_simulator_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  if (value == 0) std::abort();
  return value;
}
void CpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}
uint64_t ReadTsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}
void CalibrateTscOnce() {
  std::call_once(g_tsc_calibration_once, [] {
#if defined(__x86_64__) || defined(__i386__)
    constexpr auto window = std::chrono::milliseconds(4);
    const auto start_time = std::chrono::steady_clock::now();
    const uint64_t start_tsc = ReadTsc();
    auto end_time = start_time;
    do { CpuRelax(); end_time = std::chrono::steady_clock::now(); } while (end_time - start_time < window);
    const uint64_t end_tsc = ReadTsc();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    if (ns > 0 && end_tsc > start_tsc)
      g_tsc_ticks_per_ns.store(static_cast<double>(end_tsc - start_tsc) / static_cast<double>(ns), std::memory_order_release);
#endif
  });
}
void DelaySpinNs(uint64_t ns) {
  if (ns == 0) return;
  CalibrateTscOnce();
  const double rate = g_tsc_ticks_per_ns.load(std::memory_order_acquire);
  if (rate <= 0.0) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); return; }
#if defined(__x86_64__) || defined(__i386__)
  const uint64_t target = ReadTsc() + std::max<uint64_t>(1, static_cast<uint64_t>(rate * ns));
  while (ReadTsc() < target) CpuRelax();
#else
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
}
uint64_t RoundNs(double value) { return value <= 0.0 ? 0 : static_cast<uint64_t>(std::llround(value)); }
uint64_t XorShift64(uint64_t *state) {
  uint64_t x = *state;
  if (x == 0) x = 0x9e3779b97f4a7c15ULL;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return *state = x;
}
uint64_t LineDelayNs(const Config &c, PoolKind p, AccessKind k) {
  if (p == PoolKind::kSwcc) {
    if (k == AccessKind::kRead || k == AccessKind::kAtomicLoad) return RoundNs(c.swcc_read_ns_per_line);
    if (k == AccessKind::kFlush) return RoundNs(c.swcc_flush_ns_per_line);
    return RoundNs(c.swcc_write_ns_per_line);
  }
  if (k == AccessKind::kRead) return RoundNs(c.hwcc_read_ns_per_line);
  if (k == AccessKind::kAtomicLoad) return RoundNs(c.hwcc_atomic_load_ns);
  if (k == AccessKind::kAtomicStore) return RoundNs(c.hwcc_atomic_store_ns);
  if (k == AccessKind::kAtomicRmw) return RoundNs(c.hwcc_atomic_rmw_ns);
  return RoundNs(c.hwcc_write_ns_per_line);
}
struct Entry { uint64_t tag = 0; bool valid = false; };
struct CacheState {
  uint64_t ways = 0, sets = 0, mask = 0; std::vector<Entry> entries;
  void Reset(uint64_t capacity, uint64_t associativity) {
    capacity = std::max<uint64_t>(1, capacity); ways = std::min(std::max<uint64_t>(1, associativity), capacity);
    sets = std::max<uint64_t>(1, capacity / ways); mask = (sets & (sets - 1)) == 0 ? sets - 1 : 0;
    entries.assign(static_cast<size_t>(sets * ways), {});
  }
  bool Access(uint64_t tag) {
    if (entries.empty()) Reset(1, 1);
    Entry *set = entries.data() + static_cast<size_t>((mask ? tag & mask : tag % sets) * ways);
    uint64_t position = ways - 1;
    for (uint64_t i = 0; i < ways; ++i) { if (set[i].valid && set[i].tag == tag) { Entry hit = set[i]; for (uint64_t j=i;j>0;--j) set[j]=set[j-1]; set[0]=hit; return true; } if (!set[i].valid) { position=i; break; } }
    for (uint64_t j=position;j>0;--j) set[j]=set[j-1]; set[0] = {tag, true}; return false;
  }
};
struct ThreadState { const LatencySimulator *active = nullptr; uint64_t depth = 0, generation = 0, pending = 0, rng = 0x6a09e667f3bcc909ULL; CacheState cache; };
thread_local std::unordered_map<const LatencySimulator *, ThreadState> g_tls;
ThreadState &StateFor(const LatencySimulator *sim, const Config &c, uint64_t generation) {
  auto &state = g_tls[sim];
  if (state.generation != generation) { state = {}; state.generation = generation; if (c.cache_model == CacheModel::kPerThreadLru) state.cache.Reset(c.cache_capacity_lines, c.cache_associativity); }
  return state;
}
bool ScopeEnabled(const Config &c, ScopeKind scope) { return scope == ScopeKind::kForeground ? c.foreground_enabled : scope == ScopeKind::kMerge ? c.merge_enabled : true; }
uint64_t MakeTag(PoolKind p, uint64_t line) { return (line << 1) | (p == PoolKind::kHwcc); }
void AddStats(Stats *s, PoolKind p, uint64_t raw, uint64_t hits, uint64_t misses, uint64_t delayed) {
  auto add = [&](uint64_t &r, uint64_t &h, uint64_t &m, uint64_t &d) { r+=raw; h+=hits; m+=misses; d+=delayed; };
  if (p == PoolKind::kSwcc) add(s->swcc_raw_line_accesses,s->swcc_cache_hits,s->swcc_cache_misses,s->swcc_delayed_ns); else add(s->hwcc_raw_line_accesses,s->hwcc_cache_hits,s->hwcc_cache_misses,s->hwcc_delayed_ns);
}
std::mutex &StatsMutex() { static std::mutex mutex; return mutex; }
} // namespace

CacheModel ParseCacheModel(const std::string &value) { if (value == "none") return CacheModel::kNone; if (value == "fixed_hit_rate") return CacheModel::kFixedHitRate; if (value == "per_thread_lru") return CacheModel::kPerThreadLru; throw std::invalid_argument("unknown latency cache model: " + value); }
const char *CacheModelName(CacheModel model) { return model == CacheModel::kNone ? "none" : model == CacheModel::kFixedHitRate ? "fixed_hit_rate" : model == CacheModel::kPerThreadLru ? "per_thread_lru" : "unknown"; }
LatencySimulator &GlobalLatencySimulator() { static LatencySimulator simulator; return simulator; }
bool InstrumentationEnabledFast() { return g_instrumentation_enabled.load(std::memory_order_relaxed); }
void PrintAndResetLatencySimulatorStats(std::ostream& output, const char* tag) {
  auto& sim = GlobalLatencySimulator();
  const auto& config = sim.config();
  if (!config.enabled || !config.stats_enabled) return;
  const Stats stats = sim.TakeStatsAndReset();
  const uint64_t swcc_total = stats.TotalLineAccesses(PoolKind::kSwcc);
  const uint64_t hwcc_total = stats.TotalLineAccesses(PoolKind::kHwcc);
  const double swcc_ratio = swcc_total ? static_cast<double>(stats.CacheHits(PoolKind::kSwcc)) / swcc_total : 0.0;
  const double hwcc_ratio = hwcc_total ? static_cast<double>(stats.CacheHits(PoolKind::kHwcc)) / hwcc_total : 0.0;
  output << std::fixed << std::setprecision(6)
         << "LATENCY_SIM_STATS tag=" << (tag ? tag : "unknown")
         << " swcc_raw=" << stats.RawLineAccesses(PoolKind::kSwcc)
         << " hwcc_raw=" << stats.RawLineAccesses(PoolKind::kHwcc)
         << " swcc_hits=" << stats.CacheHits(PoolKind::kSwcc)
         << " hwcc_hits=" << stats.CacheHits(PoolKind::kHwcc)
         << " swcc_misses=" << stats.CacheMisses(PoolKind::kSwcc)
         << " hwcc_misses=" << stats.CacheMisses(PoolKind::kHwcc)
         << " swcc_hit_ratio=" << swcc_ratio << " hwcc_hit_ratio=" << hwcc_ratio
         << " swcc_delayed_ns=" << stats.DelayedNs(PoolKind::kSwcc)
         << " hwcc_delayed_ns=" << stats.DelayedNs(PoolKind::kHwcc)
         << " delayed_ns=" << stats.TotalDelayedNs()
         << " cache_model=" << CacheModelName(config.cache_model)
         << " cache_hits_enabled=" << (config.cache_hits_enabled ? 1 : 0) << '\n';
}
LatencySimulator::LatencySimulator(Config c) : config_(c), generation_(NextGeneration()) { g_instrumentation_enabled.store(c.enabled, std::memory_order_relaxed); }
void LatencySimulator::Configure(Config c) {
  if (!c.cache_line_bytes) throw std::invalid_argument("latency simulator cache_line_bytes must be > 0");
  if (c.cache_fixed_hit_rate < 0.0 || c.cache_fixed_hit_rate > 1.0) throw std::invalid_argument("latency simulator cache_fixed_hit_rate must be in [0, 1]");
  c.cache_capacity_lines = std::max<uint64_t>(1, c.cache_capacity_lines); c.cache_associativity = std::max<uint64_t>(1, c.cache_associativity);
  config_=c; g_instrumentation_enabled.store(c.enabled, std::memory_order_relaxed); generation_=NextGeneration(); if(c.enabled) CalibrateTscOnce();
}
void LatencySimulator::BeginScope(ScopeKind scope) { if (!InstrumentationEnabledFast()) return; auto &s=StateFor(this,config_,generation_); if(s.active==this){++s.depth;return;} s.active=ScopeEnabled(config_,scope)?this:nullptr; s.pending=0; s.depth=s.active==this; }
void LatencySimulator::EndScopeAndDelay() { if (!InstrumentationEnabledFast()) return; auto &s=StateFor(this,config_,generation_); if(s.active!=this) return; if(s.depth>1){--s.depth;return;} const uint64_t delay=s.pending; s.pending=0;s.depth=0;s.active=nullptr;DelaySpinNs(delay); }
void LatencySimulator::RecordLine(PoolKind p, AccessKind k, const void *a) { RecordRange(p,k,a,1); }
void LatencySimulator::RecordRange(PoolKind p, AccessKind k, const void *address, uint64_t bytes) {
  if(!InstrumentationEnabledFast()||!bytes||!address) return; const Config &c=config_; auto &s=StateFor(this,c,generation_); if(s.active!=this) return;
  const uintptr_t start=reinterpret_cast<uintptr_t>(address), end=start+static_cast<uintptr_t>(bytes-1); const uint64_t first=start/c.cache_line_bytes,last=end/c.cache_line_bytes,raw=last-first+1,miss_delay=LineDelayNs(c,p,k),hit_delay=RoundNs(c.cache_hit_extra_ns);
  if(c.cache_model==CacheModel::kNone){const uint64_t delayed=raw*miss_delay;s.pending+=delayed;if(c.stats_enabled){std::lock_guard<std::mutex> lock(StatsMutex());AddStats(&stats_,p,raw,0,raw,delayed);}return;}
  uint64_t hits=0,misses=0,delayed=0; for(uint64_t line=first;line<=last;++line){bool hit=c.cache_model==CacheModel::kFixedHitRate ? static_cast<double>(XorShift64(&s.rng)>>11)*(1.0/9007199254740992.0)<c.cache_fixed_hit_rate : s.cache.Access(MakeTag(p,line)); if(hit&&c.cache_hits_enabled){++hits;delayed+=hit_delay;}else{++misses;delayed+=miss_delay;}}
  s.pending+=delayed;if(c.stats_enabled){std::lock_guard<std::mutex> lock(StatsMutex());AddStats(&stats_,p,raw,hits,misses,delayed);}
}
Stats LatencySimulator::SnapshotStats() const { std::lock_guard<std::mutex> lock(StatsMutex()); return stats_; }
Stats LatencySimulator::TakeStatsAndReset() { std::lock_guard<std::mutex> lock(StatsMutex()); Stats out=stats_;stats_={};return out; }
uint64_t LatencySimulator::PendingDelayNsForTest() const { auto it=g_tls.find(this); return it==g_tls.end()?0:it->second.pending; }
ScopeGuard::ScopeGuard(ScopeKind scope) {
  active_ = InstrumentationEnabledFast();
  if (active_) GlobalLatencySimulator().BeginScope(scope);
}
ScopeGuard::~ScopeGuard() { if (active_) GlobalLatencySimulator().EndScopeAndDelay(); }
} // namespace latency_sim
