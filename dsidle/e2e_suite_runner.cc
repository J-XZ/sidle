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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

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
    else if (arg == "--help") { std::cout << "usage: dsidle_e2e_suite_runner --config PATH --phase e2e08_fill|e2e08_read|e2e08_delete|e2e08_verify_delete|e2e08_scan|e2e09_fill|e2e09_read|e2e09_delete|e2e09_verify_delete|e2e09_scan --node N [--bootstrap]\n"; std::exit(0); }
    else Fail("unknown argument: " + arg);
  }
  if (options.phase.empty()) Fail("--phase is required");
  return options;
}
struct Suite { uint32_t key_bytes; uint32_t value_bytes; const char* prefix; };
Suite ParseSuite(const std::string& phase) {
  if (phase.rfind("e2e08_", 0) == 0) return {8, 8, "08"};
  if (phase.rfind("e2e09_", 0) == 0) return {32, 1000, "09"};
  Fail("phase must start with e2e08_ or e2e09_");
}
bool EndsWith(const std::string& text, const char* suffix) {
  const std::string value(suffix);
  return text.size() >= value.size() && text.compare(text.size() - value.size(), value.size(), value) == 0;
}
std::string Key(const Suite& suite, uint32_t number) {
  std::string result = std::string(suite.prefix) + std::to_string(number);
  if (result.size() > suite.key_bytes) Fail("key encoding exceeds fixed size");
  result.resize(suite.key_bytes, ' ');
  return result;
}
std::string Value(const Suite& suite, uint32_t number) {
  std::string result(suite.value_bytes, static_cast<char>('!' + (number % 94)));
  const std::string prefix = "v" + std::to_string(number) + ":";
  std::memcpy(result.data(), prefix.data(), std::min(prefix.size(), result.size()));
  return result;
}
struct ScanCount {
  uint64_t count = 0;
  template <typename S, typename K> void visit_leaf(const S&, const K&, threadinfo&) {}
  bool visit_value(lcdf::Str, row_type*, threadinfo&) { ++count; return true; }
};
uint64_t Fill(Masstree::default_table& table, threadinfo& ti, const Suite& suite, uint32_t node, uint32_t nodes) {
  // Masstree's writer version locks are intentionally local to a writer.  The
  // e2e suite uses the other three processes for independent read/delete/scan
  // verification, but keeps construction of a single tree serialized.
  if (node != 0) return 0;
  query<row_type> query;
  uint64_t total = 0;
  for (uint32_t index = 0; index < kRecords; ++index) {
    const auto key = Key(suite, index), value = Value(suite, index);
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    query.run_replace(table.table(), lcdf::Str(key.data(), key.size()), lcdf::Str(value.data(), value.size()), ti);
    ++total;
  }
  return total;
}
uint64_t ReadAndVerify(Masstree::default_table& table, threadinfo& ti, const Suite& suite, uint32_t node, uint32_t nodes, bool expect_deleted) {
  query<row_type> query; uint64_t total = 0;
  for (uint32_t index = node; index < kRecords; index += nodes) {
    const bool deleted = index % kDeleteStride == 0;
    lcdf::Str actual; const auto key = Key(suite, index);
    { latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      const bool found = query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, actual, ti);
      if (expect_deleted && deleted) { if (found) Fail("deleted key was returned"); }
      else {
        if (!found) Fail("expected key is absent");
        const auto expected = Value(suite, index);
        if (actual.len != expected.size() || std::memcmp(actual.s, expected.data(), actual.len) != 0) Fail("read value differs from expected bytes");
      }
    }
    ++total;
  }
  return total;
}
uint64_t Delete(Masstree::default_table& table, threadinfo& ti, const Suite& suite, uint32_t node) {
  if (node != 1) return 0;
  query<row_type> query; uint64_t total = 0;
  for (uint32_t index = 0; index < kRecords; index += kDeleteStride) {
    const auto key = Key(suite, index);
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    query.run_remove(table.table(), lcdf::Str(key.data(), key.size()), ti); ++total;
  }
  return total;
}
uint64_t ScanAndVerify(Masstree::default_table& table, threadinfo& ti, const Suite& suite, uint32_t node) {
  if (node != 3) return 0;
  ScanCount scan; const auto first = Key(suite, 0);
  { latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    table.table().scan(lcdf::Str(first.data(), first.size()), true, scan, ti); }
  if (scan.count != kRecords - kRecords / kDeleteStride) Fail("scan count does not match post-delete key count");
  return scan.count;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = Parse(argc, argv); const auto cfg = dsidle::LoadExperimentConfig(options.config);
    if (options.node >= cfg.vm_count || cfg.vm_count < 4) Fail("suite runner requires a four-node config");
    const auto suite = ParseSuite(options.phase);
    if (cfg.fixed_key_size != suite.key_bytes || cfg.fixed_value_size != suite.value_bytes) Fail("config fixed key/value sizes do not match suite");
    auto pool = dsidle::SharedPool::Attach(cfg.shared_path, cfg.shared_size_mb << 20);
    dsidle::ConfigureCurrentSwccAllocator(pool, cfg.vm_count, options.node);
    dsidle::ReplicaDirectory replicas(pool); dsidle::ConfigureCurrentReplicaDirectory(replicas);
    latency_sim::GlobalLatencySimulator().Configure(cfg.latency_inject);
    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, 0);
    Masstree::default_table table;
    if (options.bootstrap) table.initialize(*ti, 80);
    dsidle::SharedExperimentPhaseBarrier(pool).Wait();
    if (!options.bootstrap) table.table().attach();
    const auto begin = std::chrono::steady_clock::now();
    uint64_t operations = 0;
    if (EndsWith(options.phase, "_fill")) operations = Fill(table, *ti, suite, options.node, cfg.vm_count);
    else if (EndsWith(options.phase, "_read")) operations = ReadAndVerify(table, *ti, suite, options.node, cfg.vm_count, false);
    else if (EndsWith(options.phase, "_delete")) operations = Delete(table, *ti, suite, options.node);
    else if (EndsWith(options.phase, "_verify_delete")) operations = ReadAndVerify(table, *ti, suite, options.node, cfg.vm_count, true);
    else if (EndsWith(options.phase, "_scan")) operations = ScanAndVerify(table, *ti, suite, options.node);
    else Fail("unknown suite phase action");
    dsidle::SharedExperimentPhaseBarrier(pool).Wait();
    const uint64_t duration = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
    std::cout << "E2E_TRACE_TIME_US phase=" << options.phase << " node=" << options.node << " ops=" << operations << " duration_us=" << duration << " trace_first=0 trace_workers=1 batch_ops=0\n";
    latency_sim::PrintAndResetLatencySimulatorStats(std::cout, options.phase.c_str());
    std::cout << "DSIDLE_E2E_SUITE_VERIFY suite=" << suite.prefix << " phase=" << options.phase << " node=" << options.node << " status=ok\n";
  } catch (const std::exception& error) { std::cerr << "dsidle_e2e_suite_runner: " << error.what() << '\n'; return 1; }
}
