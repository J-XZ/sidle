#include "dsidle/config.h"
#include "dsidle/latency_simulator.h"
#include "dsidle/replica_directory.h"
#include "dsidle/shared_pool.h"

#include "query_masstree.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
#include "masstree_replica_worker.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
// Keep a stable default; formal YCSB passes its recorded seed explicitly.
constexpr uint64_t kDefaultValueSeed = 0x43584c4b56545241ULL;
enum class TraceOpKind { kPut, kGet, kDelete, kScan };
struct TraceOp { TraceOpKind kind; std::string key; uint64_t len; };
struct Options { std::string config = dsidle::DefaultExperimentConfigPath(); std::string phase = "load"; uint32_t node = 0; uint32_t batch_ops = 4096; uint64_t value_seed = kDefaultValueSeed; uint64_t min_duration_sec = 0; bool bootstrap = false; };

[[noreturn]] void Fail(const std::string& text) { throw std::runtime_error(text); }
uint64_t ParseUnsigned(std::string_view text, const char* label) {
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) Fail(std::string("invalid ") + label);
  return std::stoull(std::string(text));
}
TraceOpKind ParseKind(std::string_view text) {
  if (text == "PUT") return TraceOpKind::kPut;
  if (text == "GET") return TraceOpKind::kGet;
  if (text == "DELETE") return TraceOpKind::kDelete;
  if (text == "SCAN") return TraceOpKind::kScan;
  Fail("unknown trace op: " + std::string(text));
}
TraceOp ParseTraceLine(const std::string& line, const std::filesystem::path& path, uint64_t line_number) {
  const char* data = line.data(); size_t pos = 0;
  auto fail = [&](const char* message) { Fail(path.string() + ":" + std::to_string(line_number) + ": " + message); };
  auto skip = [&] { while (pos < line.size() && (data[pos] == ' ' || data[pos] == '\t')) ++pos; };
  auto token = [&]() -> std::string_view { skip(); const size_t begin = pos; while (pos < line.size() && data[pos] != ' ' && data[pos] != '\t') ++pos; if (begin == pos) fail("missing token"); return {data + begin, pos - begin}; };
  const auto kind = ParseKind(token());
  const uint64_t key_len = ParseUnsigned(token(), "KEY_LEN");
  skip(); const size_t len_begin = pos; while (pos < line.size() && data[pos] >= '0' && data[pos] <= '9') ++pos;
  if (len_begin == pos) fail("missing LEN digits");
  const uint64_t len = ParseUnsigned(std::string_view(data + len_begin, pos - len_begin), "LEN");
  if (key_len > line.size() - pos) fail("KEY_LEN exceeds line length");
  TraceOp out{kind, std::string(data + pos, static_cast<size_t>(key_len)), len}; pos += static_cast<size_t>(key_len);
  while (pos < line.size()) { if (data[pos] != ' ' && data[pos] != '\t') fail("non-whitespace trailing bytes after key"); ++pos; }
  if ((kind == TraceOpKind::kGet || kind == TraceOpKind::kDelete) && len != 0) fail("GET/DELETE require LEN=0");
  return out;
}
class TracePrefetcher {
 public:
  TracePrefetcher(std::filesystem::path path, uint32_t batch_ops)
      : path_(std::move(path)), batch_ops_(batch_ops), input_(path_, std::ios::binary) {
    if (!input_) Fail("cannot open trace file: " + path_.string());
  }
  std::vector<TraceOp> NextBatch() {
    std::vector<TraceOp> batch;
    batch.reserve(batch_ops_);
    std::string line;
    while (batch.size() < batch_ops_ && std::getline(input_, line)) {
      ++line_no_;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') continue;
      batch.push_back(ParseTraceLine(line, path_, line_no_));
    }
    if (!input_) eof_ = true;
    return batch;
  }
  bool DoneAfter(const std::vector<TraceOp>& batch) const {
    return batch.empty() && eof_;
  }

 private:
  std::filesystem::path path_;
  uint32_t batch_ops_;
  std::ifstream input_;
  uint64_t line_no_{};
  bool eof_{false};
};
std::string FixedTraceKey(const std::string& key, uint32_t fixed_size) {
  if (key.size() > fixed_size) Fail("trace key exceeds fixed_key_size");
  std::string out = key; out.resize(fixed_size, ' '); return out;
}
std::string FixedTraceValue(uint32_t size, std::mt19937_64* rng) {
  std::string value(size, '!'); for (char& c : value) c = static_cast<char>('!' + ((*rng)() % 94)); return value;
}
Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    auto take = [&](const char* name) -> std::string { if (++i == argc) Fail(std::string("missing value for ") + name); return argv[i]; };
    if (arg == "--config") options.config = take("--config");
    else if (arg == "--phase") options.phase = take("--phase");
    else if (arg == "--node") options.node = static_cast<uint32_t>(ParseUnsigned(take("--node"), "node"));
    else if (arg == "--batch-ops") options.batch_ops = static_cast<uint32_t>(ParseUnsigned(take("--batch-ops"), "batch_ops"));
    else if (arg == "--value-seed") options.value_seed = ParseUnsigned(take("--value-seed"), "value_seed");
    else if (arg == "--min-duration-sec") options.min_duration_sec = ParseUnsigned(take("--min-duration-sec"), "min_duration_sec");
    else if (arg == "--bootstrap") options.bootstrap = true;
    else if (arg == "--help") { std::cout << "usage: dsidle_e2e_trace_runner [--config PATH] --phase NAME --node N [--batch-ops N] [--value-seed N] [--min-duration-sec N] [--bootstrap]\n"; std::exit(0); }
    else Fail("unknown argument: " + arg);
  }
  if (!options.batch_ops) Fail("batch_ops must be > 0"); return options;
}
uint64_t ReplayFile(const std::filesystem::path& path, const dsidle::ExperimentConfig& cfg,
                    Masstree::default_table* table, threadinfo* ti, uint32_t batch_ops,
                    uint64_t seed, std::atomic<uint64_t>* heartbeat,
                    std::chrono::steady_clock::time_point deadline) {
  query<row_type> query; std::mt19937_64 rng(seed); uint64_t total = 0;
  do {
    TracePrefetcher prefetcher(path, batch_ops);
    while (true) {
      const auto batch = prefetcher.NextBatch();
      if (prefetcher.DoneAfter(batch)) break;
      for (const TraceOp& op : batch) {
        const std::string key = FixedTraceKey(op.key, cfg.fixed_key_size);
        {
          latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
          switch (op.kind) {
            case TraceOpKind::kPut: { const std::string value = FixedTraceValue(cfg.fixed_value_size, &rng); query.run_replace(table->table(), lcdf::Str(key.data(), key.size()), lcdf::Str(value.data(), value.size()), *ti); break; }
            case TraceOpKind::kGet: { lcdf::Str value; (void)query.run_get1(table->table(), lcdf::Str(key.data(), key.size()), 0, value, *ti); break; }
            case TraceOpKind::kDelete: (void)query.run_remove(table->table(), lcdf::Str(key.data(), key.size()), *ti); break;
            case TraceOpKind::kScan: {
              lcdf::Json request =
                  lcdf::Json::array(0, 0, lcdf::Str(key.data(), key.size()), op.len);
              query.run_scan(table->table(), request, *ti);
              break;
            }
          }
        }
        ++total;
        // Publish one shared heartbeat increment per replayed physical command.
        // This is workload progress metadata, not a hardware-simulation record.
        heartbeat->fetch_add(std::uint64_t{1}, std::memory_order_relaxed);
      }
    }
  } while (std::chrono::steady_clock::now() < deadline);
  // The trailing RCU drain reads shared epoch state; settle only after
  // rcu_drain() returned and every RCU/allocator guard is released.
  {
    latency_sim::ScopeGuard drain_scope(latency_sim::ScopeKind::kForeground);
    ti->rcu_drain();
  }
  return total;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseOptions(argc, argv); const auto cfg = dsidle::LoadExperimentConfig(options.config);
    if (options.node >= cfg.vm_count) Fail("node is outside vm.count");
#ifndef NDEBUG
    if (cfg.latency_inject.fixed_latency.enabled)
      Fail("enabled latency injection requires a RelWithDebInfo/non-Debug build");
#endif
    const dsidle::PoolLayout expected_layout{
        cfg.shared_size_mb << 20, cfg.hwcc.offset_mb << 20,
        cfg.hwcc.size_mb << 20, cfg.swcc.offset_mb << 20,
        cfg.swcc.size_mb << 20};
    auto pool =
        dsidle::SharedPool::Attach(cfg.shared_path, expected_layout);
    dsidle::ConfigureCurrentSwccAllocator(pool, cfg.vm_count, options.node);
    dsidle::ReplicaDirectory replicas(pool); dsidle::ConfigureCurrentReplicaDirectory(replicas);
    replicas.SetBudgetBytes(cfg.replica_budget_mb << 20);
    dsidle::ConfigureLatencySimulatorForPool(pool, cfg.latency_inject,
                                             options.node);
    sidle::strategy_manager = sidle::sidle_strategy(
        cfg.replica_budget_mb, cfg.hot_percentage_seed, false);
    Masstree::node_base<Masstree::default_query_table_params>::strategy_manager =
        &sidle::strategy_manager;
    threadinfo* bootstrap_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
    Masstree::default_table table;
    if (options.bootstrap) {
      // Main-thread bootstrap touches HWCC/SWCC; settle in its own short
      // scope before any barrier or worker start.
      latency_sim::ScopeGuard bootstrap_scope(latency_sim::ScopeKind::kOther);
      table.initialize(*bootstrap_ti, cfg.hot_percentage_seed);
    }
    // Node zero publishes the root before entering; every other VM waits
    // before attaching. This is an ivshmem-backed phase barrier, excluded
    // from the workload timer below.
    auto phase_barrier = [&pool] {
      latency_sim::ScopeGuard layout_scope(latency_sim::ScopeKind::kOther);
      return dsidle::SharedExperimentPhaseBarrier(pool);
    }();
    {
      latency_sim::ScopeGuard barrier_scope(latency_sim::ScopeKind::kOther);
      phase_barrier.Wait();
    }
    if (!options.bootstrap) {
      latency_sim::ScopeGuard attach_scope(latency_sim::ScopeKind::kOther);
      table.table().attach();
    }
    std::uint64_t epoch_slots_per_vm = 0;
    {
      latency_sim::ScopeGuard layout_scope(latency_sim::ScopeKind::kOther);
      epoch_slots_per_vm =
          latency_sim::FixedLatencyMemoryLoad(
              latency_sim::PoolKind::kHwcc,
              &pool.static_layout()->epoch_slot_count) /
          cfg.vm_count;
    }
    if (epoch_slots_per_vm < cfg.foreground_worker_count_per_vm + 4)
      Fail("pool epoch slots must reserve four SIDLE replica workers per VM");
    auto& thresholds = *sidle::strategy_manager.get_threshold_manager();
    Masstree::replica_workers<Masstree::default_query_table_params> replica_workers(
        table.table(), pool, replicas, thresholds, cfg.vm_count, options.node,
        cfg.foreground_worker_count_per_vm, std::chrono::milliseconds(10),
        std::chrono::milliseconds(1000), std::chrono::milliseconds(1000));
    if (!replica_workers.PinRoot(*bootstrap_ti))
      Fail("failed to pin the canonical root replica");
    replica_workers.Start();
    const uint32_t workers = cfg.foreground_worker_count_per_vm; const uint32_t first = options.node * workers;
    std::atomic<uint64_t> heartbeat{0}, heartbeat_stop{false};
    std::mutex heartbeat_mutex, worker_mutex;
    std::condition_variable heartbeat_cv, worker_cv;
    std::vector<std::thread> threads;
    std::vector<uint64_t> counts(workers);
    std::exception_ptr worker_failure;
    uint32_t ready = 0, done = 0;
    bool start = false;
    std::chrono::steady_clock::time_point started, deadline;
    std::thread heartbeat_thread([&] {
      {
        std::unique_lock<std::mutex> lock(worker_mutex);
        worker_cv.wait(lock, [&] { return start; });
      }
      uint64_t previous = 0;
      while (!heartbeat_stop.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(heartbeat_mutex);
        heartbeat_cv.wait_for(lock, std::chrono::seconds(1), [&] {
          return heartbeat_stop.load(std::memory_order_acquire);
        });
        if (heartbeat_stop.load(std::memory_order_acquire)) break;
        const uint64_t current =
            heartbeat.load(std::memory_order_relaxed);
        const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
        std::cout << "E2E_TRACE_HEARTBEAT phase=" << options.phase << " node=" << options.node << " ops=" << (current - previous) << " total=" << current << " elapsed_s=" << elapsed << '\n' << std::flush;
        previous = current;
      }
    });
    auto replay_worker = [&](uint32_t worker) {
      threadinfo* ti = nullptr;
      try {
        // Short worker-local binding scope: covers the typed HWCC layout
        // reads in the binding plus thread initialization, and settles
        // before the ready/cv handshake or any per-operation scope.  Each
        // replayed operation establishes its own top-level foreground scope
        // inside ReplayFile.
        {
          latency_sim::ScopeGuard bind_scope(
              latency_sim::ScopeKind::kForeground);
          dsidle::ConfigureCurrentSwccAllocator(pool, cfg.vm_count,
                                                options.node);
          dsidle::ConfigureCurrentReplicaDirectory(replicas);
          ti = threadinfo::make(threadinfo::TI_MAIN, worker);
        }
      } catch (...) {
        std::lock_guard<std::mutex> lock(worker_mutex);
        if (!worker_failure) worker_failure = std::current_exception();
      }
      {
        std::unique_lock<std::mutex> lock(worker_mutex);
        ++ready;
        worker_cv.notify_all();
        worker_cv.wait(lock, [&] { return start; });
      }
      if (ti) {
        try {
        counts[worker] = ReplayFile(
            std::filesystem::path(cfg.trace_dir) /
                ("worker" + std::to_string(first + worker) + ".txt"),
            cfg, &table, ti, options.batch_ops,
            options.value_seed ^ (uint64_t(first + worker) << 32) ^
                uint64_t(worker),
            &heartbeat, deadline);
        } catch (...) {
          std::lock_guard<std::mutex> lock(worker_mutex);
          if (!worker_failure) worker_failure = std::current_exception();
        }
      }
      {
        std::lock_guard<std::mutex> lock(worker_mutex);
        ++done;
        worker_cv.notify_all();
      }
    };
    for (uint32_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, worker] { replay_worker(worker); });
    {
      std::unique_lock<std::mutex> lock(worker_mutex);
      worker_cv.wait(lock, [&] { return ready == workers; });
    }
    const auto barrier_begin = std::chrono::steady_clock::now();
    {
      latency_sim::ScopeGuard barrier_scope(latency_sim::ScopeKind::kOther);
      phase_barrier.Wait();
    }
    const auto barrier_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - barrier_begin).count();
    started = std::chrono::steady_clock::now();
    deadline = started + std::chrono::seconds(options.min_duration_sec);
    {
      std::lock_guard<std::mutex> lock(worker_mutex);
      start = true;
      worker_cv.notify_all();
    }
    {
      std::unique_lock<std::mutex> lock(worker_mutex);
      worker_cv.wait(lock, [&] { return done == workers; });
    }
    const auto ended = std::chrono::steady_clock::now();
    heartbeat_stop.store(std::uint64_t{1}, std::memory_order_release);
    heartbeat_cv.notify_one();
    for (auto& thread : threads) thread.join();
    heartbeat_thread.join();
    replica_workers.Stop();
    if (worker_failure) std::rethrow_exception(worker_failure);
    // All foreground and local replica workers are drained. Every VM crosses
    // this barrier before the phase is reported complete.
    const auto end_barrier_begin = std::chrono::steady_clock::now();
    {
      latency_sim::ScopeGuard barrier_scope(latency_sim::ScopeKind::kOther);
      phase_barrier.Wait();
    }
    const auto end_barrier_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - end_barrier_begin).count();
    const uint64_t duration = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ended - started).count());
    uint64_t total = 0; for (const auto count : counts) total += count;
    std::cout << "E2E_BARRIER_TIME_US node=" << options.node
              << " role=" << (options.node == 0 ? "leader" : "follower")
              << " phase_id=1 duration_us=" << barrier_duration
              << " phase=" << options.phase << "_start\n";
    std::cout << "E2E_BARRIER_TIME_US node=" << options.node
              << " role=" << (options.node == 0 ? "leader" : "follower")
              << " phase_id=2 duration_us=" << end_barrier_duration
              << " phase=" << options.phase << "_end\n";
    std::cout << "E2E_TRACE_TIME_US phase=" << options.phase << " node=" << options.node << " ops=" << total << " duration_us=" << duration << " trace_first=" << first << " trace_workers=" << workers << " batch_ops=" << options.batch_ops << '\n';
    std::uint64_t hwcc_bytes = 0, swcc_bytes = 0;
    {
      latency_sim::ScopeGuard stats_scope(latency_sim::ScopeKind::kOther);
      hwcc_bytes = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &pool.header()->hwcc_bytes);
      swcc_bytes = latency_sim::FixedLatencyMemoryLoad(
          latency_sim::PoolKind::kHwcc, &pool.header()->swcc_bytes);
    }
    std::cout << "DSIDLE_MEMORY_STATS hwcc_bytes=" << hwcc_bytes
              << " swcc_bytes=" << swcc_bytes
              << " replica_bytes=" << replicas.LocalBytes() << '\n';
  } catch (const std::exception& error) { std::cerr << "dsidle_e2e_trace_runner: " << error.what() << '\n'; return 1; }
}
