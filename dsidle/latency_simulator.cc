#include "dsidle/latency_simulator.h"
#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif
namespace latency_sim { namespace {
std::atomic<bool> gate{false}; std::atomic<uint64_t> next_generation{0}; std::once_flag once; std::atomic<double> ticks{0}; std::mutex stats_mu;
uint64_t generation(){ auto n=next_generation.fetch_add(1)+1; if(!n) std::abort(); return n; }
void pause_cpu(){
#if defined(__x86_64__) || defined(__i386__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}
uint64_t tsc(){
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}
void calibrate(){ std::call_once(once,[]{ auto a=std::chrono::steady_clock::now(); auto b=a; const auto c=tsc(); do { pause_cpu(); b=std::chrono::steady_clock::now(); } while(b-a<std::chrono::milliseconds(4)); auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count(); const auto end=tsc(); if(ns&&end>c) ticks.store(double(end-c)/ns); }); }
struct State { uint64_t gen=0, depth=0, pending=0; bool active=false; }; thread_local State state;
uint64_t delay(const Config& c,PoolKind p,AccessKind k){ double n=0; if(p==PoolKind::kSwcc) n=k==AccessKind::kRead?c.swcc_read_ns_per_line:k==AccessKind::kFlush?c.swcc_flush_ns_per_line:c.swcc_write_ns_per_line; else n=k==AccessKind::kRead?c.hwcc_read_ns_per_line:k==AccessKind::kAtomicLoad?c.hwcc_atomic_load_ns:k==AccessKind::kAtomicStore?c.hwcc_atomic_store_ns:k==AccessKind::kAtomicRmw?c.hwcc_atomic_rmw_ns:c.hwcc_write_ns_per_line; return n>0?uint64_t(std::llround(n)):0; }
}
CacheModel ParseCacheModel(const std::string& s){ if(s=="none")return CacheModel::kNone; if(s=="fixed_hit_rate")return CacheModel::kFixedHitRate; if(s=="per_thread_lru")return CacheModel::kPerThreadLru; throw std::invalid_argument("unknown latency cache model: "+s); }
const char* CacheModelName(CacheModel m){ return m==CacheModel::kNone?"none":m==CacheModel::kFixedHitRate?"fixed_hit_rate":"per_thread_lru"; }
LatencySimulator& GlobalLatencySimulator(){ static LatencySimulator s; return s; } bool InstrumentationEnabledFast(){return gate.load(std::memory_order_relaxed);}
LatencySimulator::LatencySimulator(Config c):config_(c),generation_(generation()){gate.store(c.enabled,std::memory_order_relaxed);}
void LatencySimulator::Configure(Config c){if(!c.cache_line_bytes||c.cache_fixed_hit_rate<0||c.cache_fixed_hit_rate>1)throw std::invalid_argument("invalid latency configuration");config_=c;generation_=generation();gate.store(c.enabled,std::memory_order_relaxed);if(c.enabled)calibrate();}
void LatencySimulator::BeginScope(ScopeKind s){if(!InstrumentationEnabledFast())return;if(state.gen!=generation_){state={generation_,0,0,false};}if(state.active){++state.depth;return;}state.active=s!=ScopeKind::kForeground||config_.foreground_enabled; if(s==ScopeKind::kMerge)state.active=config_.merge_enabled;state.depth=state.active;state.pending=0;}
void LatencySimulator::EndScopeAndDelay(){if(!state.active||--state.depth)return;auto n=state.pending;state={generation_,0,0,false};calibrate();auto r=ticks.load();if(r>0){auto end=tsc()+uint64_t(std::max(1.0,r*n));while(tsc()<end)pause_cpu();}}
void LatencySimulator::RecordLine(PoolKind p,AccessKind k,const void* a){RecordRange(p,k,a,1);} void LatencySimulator::RecordRange(PoolKind p,AccessKind k,const void* a,uint64_t b){if(!InstrumentationEnabledFast()||!state.active||!a||!b)return;auto f=reinterpret_cast<uintptr_t>(a)/config_.cache_line_bytes,l=(reinterpret_cast<uintptr_t>(a)+b-1)/config_.cache_line_bytes,n=l-f+1,d=n*delay(config_,p,k);state.pending+=d;if(config_.stats_enabled){std::lock_guard<std::mutex>x(stats_mu);auto& raw=p==PoolKind::kSwcc?stats_.swcc_raw_line_accesses:stats_.hwcc_raw_line_accesses;auto& miss=p==PoolKind::kSwcc?stats_.swcc_cache_misses:stats_.hwcc_cache_misses;auto& ns=p==PoolKind::kSwcc?stats_.swcc_delayed_ns:stats_.hwcc_delayed_ns;raw+=n;miss+=n;ns+=d;}}
Stats LatencySimulator::SnapshotStats()const{std::lock_guard<std::mutex>x(stats_mu);return stats_;} Stats LatencySimulator::TakeStatsAndReset(){std::lock_guard<std::mutex>x(stats_mu);auto r=stats_;stats_={};return r;} uint64_t LatencySimulator::PendingDelayNsForTest()const{return state.pending;}
} // namespace latency_sim
