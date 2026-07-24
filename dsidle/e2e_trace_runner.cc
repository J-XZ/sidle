#include "dsidle/config.h"
#include "dsidle/latency_simulator.h"
#include "dsidle/replica_directory.h"
#include "dsidle/shared_pool.h"

#include "query_masstree.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
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
enum class TraceOpKind { kPut, kGet, kDelete, kScan };
struct TraceOp { TraceOpKind kind; std::string key; uint64_t len; };
struct Options { std::string config = dsidle::DefaultExperimentConfigPath(); std::string phase = "load"; uint32_t node = 0; uint32_t batch_ops = 4096; bool bootstrap = false; };

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
    else if (arg == "--bootstrap") options.bootstrap = true;
    else if (arg == "--help") { std::cout << "usage: dsidle_e2e_trace_runner [--config PATH] --phase NAME --node N [--batch-ops N] [--bootstrap]\n"; std::exit(0); }
    else Fail("unknown argument: " + arg);
  }
  if (!options.batch_ops) Fail("batch_ops must be > 0"); return options;
}
struct ScanCounter {
  uint64_t limit; uint64_t seen = 0;
  template <typename S, typename K> void visit_leaf(const S&, const K&, threadinfo&) {}
  bool visit_value(lcdf::Str, row_type*, threadinfo&) { return limit == 0 || ++seen < limit; }
};

uint64_t ReplayFile(const std::filesystem::path& path, const dsidle::ExperimentConfig& cfg,
                    Masstree::default_table* table, threadinfo* ti, uint32_t batch_ops,
                    uint64_t seed, std::atomic<uint64_t>* heartbeat) {
  std::ifstream input(path, std::ios::binary); if (!input) Fail("cannot open trace file: " + path.string());
  query<row_type> query; std::mt19937_64 rng(seed); uint64_t total = 0, line_no = 0;
  std::string line;
  while (std::getline(input, line)) {
    ++line_no; if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos || line.find_first_not_of(" \t") == line.find('#')) continue;
    const TraceOp op = ParseTraceLine(line, path, line_no); const std::string key = FixedTraceKey(op.key, cfg.fixed_key_size);
    { latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      switch (op.kind) {
        case TraceOpKind::kPut: { const std::string value = FixedTraceValue(cfg.fixed_value_size, &rng); query.run_replace(table->table(), lcdf::Str(key.data(), key.size()), lcdf::Str(value.data(), value.size()), *ti); break; }
        case TraceOpKind::kGet: { lcdf::Str value; (void)query.run_get1(table->table(), lcdf::Str(key.data(), key.size()), 0, value, *ti); break; }
        case TraceOpKind::kDelete: (void)query.run_remove(table->table(), lcdf::Str(key.data(), key.size()), *ti); break;
        case TraceOpKind::kScan: { ScanCounter counter{op.len}; (void)table->table().scan(lcdf::Str(key.data(), key.size()), true, counter, *ti); break; }
      }
    }
    ++total; heartbeat->fetch_add(1, std::memory_order_relaxed);
    (void)batch_ops;
  }
  ti->rcu_drain(); return total;
}
} // namespace

int main(int argc, char** argv) {
  try {
    const Options options = ParseOptions(argc, argv); const auto cfg = dsidle::LoadExperimentConfig(options.config);
    if (options.node >= cfg.vm_count) Fail("node is outside vm.count");
#ifndef NDEBUG
    if (cfg.latency_inject.enabled)
      Fail("enabled latency injection requires a RelWithDebInfo/non-Debug build");
#endif
    auto pool = dsidle::SharedPool::Attach(cfg.shared_path, cfg.shared_size_mb << 20);
    dsidle::ConfigureCurrentSwccAllocator(pool, cfg.vm_count, options.node);
    dsidle::ReplicaDirectory replicas(pool); dsidle::ConfigureCurrentReplicaDirectory(replicas);
    latency_sim::GlobalLatencySimulator().Configure(cfg.latency_inject);
    threadinfo* bootstrap_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
    Masstree::default_table table;
    if (options.bootstrap) table.initialize(*bootstrap_ti, 80);
    // Node zero publishes the root before entering; every other VM waits
    // before attaching. This is an ivshmem-backed phase barrier, excluded
    // from the workload timer below.
    dsidle::SharedExperimentPhaseBarrier(pool).Wait();
    if (!options.bootstrap) table.table().attach();
    const uint32_t workers = cfg.foreground_worker_count_per_vm; const uint32_t first = options.node * workers;
    std::atomic<uint64_t> heartbeat{0}, heartbeat_stop{false}; std::mutex heartbeat_mutex; std::condition_variable heartbeat_cv; std::vector<std::thread> threads; std::vector<uint64_t> counts(workers); std::exception_ptr worker_failure; std::mutex worker_failure_mutex;
    const auto started = std::chrono::steady_clock::now();
    std::thread heartbeat_thread([&] {
      uint64_t previous = 0;
      while (!heartbeat_stop.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(heartbeat_mutex);
        heartbeat_cv.wait_for(lock, std::chrono::seconds(1), [&] { return heartbeat_stop.load(std::memory_order_acquire); });
        if (heartbeat_stop.load(std::memory_order_acquire)) break;
        const uint64_t current = heartbeat.load(std::memory_order_relaxed);
        const uint64_t elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
        std::cout << "E2E_TRACE_HEARTBEAT phase=" << options.phase << " node=" << options.node << " ops=" << (current - previous) << " total=" << current << " elapsed_s=" << elapsed << '\n' << std::flush;
        previous = current;
      }
    });
    auto replay_worker = [&](uint32_t worker) {
      try {
        dsidle::ConfigureCurrentSwccAllocator(pool, cfg.vm_count, options.node); dsidle::ConfigureCurrentReplicaDirectory(replicas);
        threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, worker);
        counts[worker] = ReplayFile(std::filesystem::path(cfg.trace_dir) / ("worker" + std::to_string(first + worker) + ".txt"), cfg, &table, ti, options.batch_ops, 0x43584c4b56545241ULL ^ (uint64_t(first + worker) << 32), &heartbeat);
      } catch (...) { std::lock_guard<std::mutex> lock(worker_failure_mutex); if (!worker_failure) worker_failure = std::current_exception(); }
    };
    for (uint32_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, worker] { replay_worker(worker); });
    for (auto& thread : threads) thread.join();
    heartbeat_stop.store(true, std::memory_order_release); heartbeat_cv.notify_one(); heartbeat_thread.join();
    if (worker_failure) std::rethrow_exception(worker_failure);
    const uint64_t duration = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    uint64_t total = 0; for (const auto count : counts) total += count;
    std::cout << "E2E_TRACE_TIME_US phase=" << options.phase << " node=" << options.node << " ops=" << total << " duration_us=" << duration << " trace_first=" << first << " trace_workers=" << workers << " batch_ops=" << options.batch_ops << '\n';
    latency_sim::PrintAndResetLatencySimulatorStats(std::cout, options.phase.c_str());
    std::cout << "DSIDLE_MEMORY_STATS hwcc_bytes=" << pool.header()->hwcc_bytes << " swcc_bytes=" << pool.header()->swcc_bytes << " replica_bytes=" << replicas.LocalBytes() << '\n';
  } catch (const std::exception& error) { std::cerr << "dsidle_e2e_trace_runner: " << error.what() << '\n'; return 1; }
}
