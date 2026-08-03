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
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kRecords = 100000;
constexpr uint32_t kDeleteStride = 10;
struct Options { std::string config = dsidle::DefaultExperimentConfigPath(); std::string phase; uint32_t node = 0; bool bootstrap = false; };
[[noreturn]] void Fail(const std::string& text) { throw std::runtime_error(text); }
uint32_t Number(const std::string& text, const char* name) {
  if (text.empty()) Fail(std::string("missing ") + name);
  uint64_t result = 0;
  for (char value : text) { if (value < '0' || value > '9') Fail(std::string("invalid ") + name); result = result * 10 + unsigned(value - '0'); }
  if (result > UINT32_MAX) Fail(std::string("out of range ") + name);
  return static_cast<uint32_t>(result);
}
Options Parse(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string arg(argv[index]);
    auto take = [&](const char* name) { if (++index == argc) Fail(std::string("missing value for ") + name); return std::string(argv[index]); };
    if (arg == "--config") options.config = take("--config");
    else if (arg == "--phase") options.phase = take("--phase");
    else if (arg == "--node") options.node = Number(take("--node"), "node");
    else if (arg == "--bootstrap") options.bootstrap = true;
    else if (arg == "--help") { std::cout << "usage: dsidle_e2e_suite_runner --config PATH --phase e2e08_fill|e2e08_read|e2e09_fill|e2e09_update|e2e09_read --node N [--bootstrap]\n"; std::exit(0); }
    else Fail("unknown argument: " + arg);
  }
  if (options.phase.empty()) Fail("--phase is required");
  return options;
}
enum class SuiteKind { k08, k09 };
struct Suite {
  SuiteKind kind;
  uint32_t key_bytes;
  uint32_t value_bytes;
  const char* prefix;
};
Suite ParseSuite(const std::string& phase) {
  if (phase.rfind("e2e08_", 0) == 0)
    return {SuiteKind::k08, 8, 8, "08"};
  if (phase.rfind("e2e09_", 0) == 0)
    return {SuiteKind::k09, 32, 1000, "09"};
  Fail("phase must start with e2e08_ or e2e09_");
}
uint64_t StartForPart(uint64_t count, uint32_t parts, uint32_t part) { return count * part / parts; }
uint64_t CountForPart(uint64_t count, uint32_t parts, uint32_t part) { return StartForPart(count, parts, part + 1) - StartForPart(count, parts, part); }
char Base36(uint64_t value) { return static_cast<char>(value < 10 ? '0' + value : 'A' + (value - 10)); }
uint64_t SplitMix64(uint64_t value) { value += 0x9e3779b97f4a7c15ULL; value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL; value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL; return value ^ (value >> 31U); }
std::string Key(const Suite& suite, uint64_t index) {
  std::string result(suite.key_bytes, '0');
  result[0] = suite.kind == SuiteKind::k08 ? 'k' : 'u';
  for (uint32_t pos = suite.key_bytes - 1; pos > 0; --pos) { result[pos] = Base36(index % 36ULL); index /= 36ULL; }
  return result;
}
std::string Value(const Suite& suite, uint64_t index, uint64_t generation) {
  if (suite.kind == SuiteKind::k08) { std::string result(suite.value_bytes, '0'); for (uint32_t pos = suite.value_bytes; pos > 0; --pos) { result[pos - 1] = Base36(index % 36ULL); index /= 36ULL; } return result; }
  std::string result(suite.value_bytes, '!'); uint64_t state = SplitMix64(index ^ (generation << 48U) ^ 0xC209ULL);
  for (uint64_t i = 0; i < result.size(); ++i) { if ((i & 7ULL) == 0) state = SplitMix64(state + i + generation); const uint64_t byte = (state >> ((i & 7ULL) * 8ULL)) & 0xffULL; result[i] = static_cast<char>('!' + (byte % 94ULL)); }
  return result;
}
uint64_t ReadIndex(const Suite& suite, uint32_t node, uint32_t worker, uint64_t seq) {
  const uint64_t seed =
      (suite.kind == SuiteKind::k08 ? 0xE2080000ULL : 0xE2090000ULL) ^
      (uint64_t(node) << 32U) ^ (uint64_t(worker + 1) << 16U) ^ seq;
  return SplitMix64(seed) % kRecords;
}
struct RunResult {
  uint64_t operations{};
  uint64_t duration_us{};
  uint64_t start_barrier_us{};
};
RunResult RunWorkers(
    Masstree::default_table& table, dsidle::SharedPool& pool,
    dsidle::ReplicaDirectory& replicas,
    dsidle::SharedPhaseBarrierView& phase_barrier, uint32_t vm_count,
    uint32_t node, uint32_t workers,
    const std::function<uint64_t(uint32_t, threadinfo&)>& operation) {
  std::vector<std::thread> threads;
  std::vector<uint64_t> counts(workers);
  std::exception_ptr failure;
  std::mutex mutex;
  std::condition_variable cv;
  uint32_t ready = 0, done = 0;
  bool start = false;
  for (uint32_t worker = 0; worker < workers; ++worker)
    threads.emplace_back([&, worker] {
      threadinfo* ti = nullptr;
      try {
        dsidle::ConfigureCurrentSwccAllocator(pool, vm_count, node);
        dsidle::ConfigureCurrentReplicaDirectory(replicas);
        ti = threadinfo::make(threadinfo::TI_MAIN, worker);
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failure) failure = std::current_exception();
      }
      {
        std::unique_lock<std::mutex> lock(mutex);
        ++ready;
        cv.notify_all();
        cv.wait(lock, [&] { return start; });
      }
      if (ti) {
        try {
          counts[worker] = operation(worker, *ti);
        } catch (...) {
          std::lock_guard<std::mutex> lock(mutex);
          if (!failure) failure = std::current_exception();
        }
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++done;
        cv.notify_all();
      }
    });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return ready == workers; });
  }
  const auto barrier_begin = std::chrono::steady_clock::now();
  phase_barrier.Wait();
  const auto barrier_end = std::chrono::steady_clock::now();
  const auto begin = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(mutex);
    start = true;
    cv.notify_all();
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return done == workers; });
  }
  const auto end = std::chrono::steady_clock::now();
  for (auto& thread : threads) thread.join();
  if (failure) std::rethrow_exception(failure);
  RunResult result;
  for (const auto count : counts) result.operations += count;
  result.duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           end - begin).count();
  result.start_barrier_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          barrier_end - barrier_begin).count();
  return result;
}
RunResult Put(Masstree::default_table& table, dsidle::SharedPool& pool,
             dsidle::ReplicaDirectory& replicas, const Suite& suite,
             dsidle::SharedPhaseBarrierView& phase_barrier,
             uint32_t node, uint32_t nodes, uint32_t workers,
             uint64_t generation) {
  const uint64_t node_start = StartForPart(kRecords, nodes, node), node_count = CountForPart(kRecords, nodes, node);
  return RunWorkers(table, pool, replicas, phase_barrier, nodes, node, workers, [&](uint32_t worker, threadinfo& ti) { query<row_type> query; const uint64_t start = StartForPart(node_count, workers, worker), count = CountForPart(node_count, workers, worker); for (uint64_t i = 0; i < count; ++i) { const auto key = Key(suite, node_start + start + i), value = Value(suite, node_start + start + i, generation); latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground); query.run_replace(table.table(), lcdf::Str(key.data(), key.size()), lcdf::Str(value.data(), value.size()), ti); } return count; });
}
RunResult ReadAndVerify(Masstree::default_table& table,
                       dsidle::SharedPool& pool,
                       dsidle::ReplicaDirectory& replicas,
                       dsidle::SharedPhaseBarrierView& phase_barrier,
                       const Suite& suite, uint32_t node, uint32_t nodes,
                       uint32_t workers, uint64_t generation) {
  const uint64_t node_count = CountForPart(kRecords, nodes, node);
  return RunWorkers(table, pool, replicas, phase_barrier, nodes, node, workers, [&](uint32_t worker, threadinfo& ti) { query<row_type> query; const uint64_t start = StartForPart(node_count, workers, worker), count = CountForPart(node_count, workers, worker); for (uint64_t i = 0; i < count; ++i) { const uint64_t index = ReadIndex(suite, node, worker, start + i); const auto key = Key(suite, index), expected = Value(suite, index, generation); lcdf::Str actual; latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground); const bool found = query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, actual, ti); if (!found || actual.len != expected.size() || std::memcmp(actual.s, expected.data(), actual.len) != 0) Fail("cross-VM read value differs: index=" + std::to_string(index) + " found=" + std::to_string(found) + " actual_bytes=" + std::to_string(actual.len) + " expected_bytes=" + std::to_string(expected.size())); } return count; });
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = Parse(argc, argv); const auto cfg = dsidle::LoadExperimentConfig(options.config);
    if (options.node >= cfg.vm_count || cfg.vm_count < 4) Fail("suite runner requires a four-node config");
    const auto suite = ParseSuite(options.phase);
    if (cfg.fixed_key_size != suite.key_bytes || cfg.fixed_value_size != suite.value_bytes) Fail("config fixed key/value sizes do not match suite");
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
    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, 0);
    Masstree::default_table table;
    if (options.bootstrap) table.initialize(*ti, cfg.hot_percentage_seed);
    auto phase_barrier = dsidle::SharedExperimentPhaseBarrier(pool);
    phase_barrier.Wait();
    if (!options.bootstrap) table.table().attach();
    const auto epoch_slots_per_vm =
        pool.static_layout()->epoch_slot_count / cfg.vm_count;
    if (epoch_slots_per_vm < cfg.foreground_worker_count_per_vm + 4)
      Fail("pool epoch slots must reserve four SIDLE replica workers per VM");
    auto& thresholds = *sidle::strategy_manager.get_threshold_manager();
    Masstree::replica_workers<Masstree::default_query_table_params>
        replica_workers(
            table.table(), pool, replicas, thresholds, cfg.vm_count,
            options.node, cfg.foreground_worker_count_per_vm,
            std::chrono::milliseconds(10), std::chrono::milliseconds(1000),
            std::chrono::milliseconds(1000));
    if (!replica_workers.PinRoot(*ti))
      Fail("failed to pin the canonical root replica");
    replica_workers.Start();
    RunResult result;
    if (options.phase == "e2e08_fill") result = Put(table, pool, replicas, suite, phase_barrier, options.node, cfg.vm_count, cfg.foreground_worker_count_per_vm, 0);
    else if (options.phase == "e2e08_read") result = ReadAndVerify(table, pool, replicas, phase_barrier, suite, options.node, cfg.vm_count, cfg.foreground_worker_count_per_vm, 0);
    else if (options.phase == "e2e09_fill") result = Put(table, pool, replicas, suite, phase_barrier, options.node, cfg.vm_count, cfg.foreground_worker_count_per_vm, 0);
    else if (options.phase == "e2e09_update") result = Put(table, pool, replicas, suite, phase_barrier, options.node, cfg.vm_count, cfg.foreground_worker_count_per_vm, 1);
    else if (options.phase == "e2e09_read") result = ReadAndVerify(table, pool, replicas, phase_barrier, suite, options.node, cfg.vm_count, cfg.foreground_worker_count_per_vm, 1);
    else Fail("unknown suite phase action");
    replica_workers.Stop();
    // Stop local background workers before the drain barrier. The remote log
    // is validated only after every VM has stopped producing events.
    const auto end_barrier_begin = std::chrono::steady_clock::now();
    phase_barrier.Wait();
    const auto end_barrier_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - end_barrier_begin).count();
    const char* role = options.node == 0 ? "leader" : "follower";
    std::cout << "E2E_BARRIER_TIME_US node=" << options.node
              << " role=" << role << " phase_id=1 duration_us="
              << result.start_barrier_us << " phase=" << options.phase
              << "_start\n";
    std::cout << "E2E_BARRIER_TIME_US node=" << options.node
              << " role=" << role << " phase_id=2 duration_us="
              << end_barrier_us << " phase=" << options.phase << "_end\n";
    std::cout << "E2E_TRACE_TIME_US phase=" << options.phase << " node=" << options.node << " ops=" << result.operations << " duration_us=" << result.duration_us << " trace_first=" << options.node * cfg.foreground_worker_count_per_vm << " trace_workers=" << cfg.foreground_worker_count_per_vm << " batch_ops=0\n";
    latency_sim::PrintAndResetLatencySimulatorStats(std::cout, options.phase.c_str());
    std::cout << "DSIDLE_MEMORY_STATS hwcc_bytes=" << pool.header()->hwcc_bytes
              << " swcc_bytes=" << pool.header()->swcc_bytes
              << " replica_bytes=" << replicas.LocalBytes() << '\n';
    std::cout << "DSIDLE_E2E_SUITE_VERIFY suite=" << suite.prefix << " phase=" << options.phase << " node=" << options.node << " status=ok\n";
  } catch (const std::exception& error) { std::cerr << "dsidle_e2e_suite_runner: " << error.what() << '\n'; return 1; }
}
