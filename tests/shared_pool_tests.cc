#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"
#include "dsidle/latency_simulator.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void TestRemoteInstrumentationAcrossMappings() {
  char path[] = "/tmp/dsidle-remote-pool-test.XXXXXX";
  const int placeholder = mkstemp(path);
  assert(placeholder >= 0);
  close(placeholder);
  unlink(path);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 64ULL << 20,
                                  64ULL << 20, 64ULL << 20};
  auto pool = dsidle::SharedPool::Create(path, layout);
  dsidle::InitializePoolMetadata(pool, {2, 1, 16, true, 16, 192});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 2);
  dsidle::FinalizePoolInitialization(pool);

  latency_sim::Config config;
  auto& remote = config.remote_cache_invalidation;
  remote.enabled = true;
  remote.node_count = 2;
  remote.cache_line_bytes = 64;
  remote.cache_size_bytes_per_node = 1024;
  remote.cache_instances_per_node = 1;
  remote.associativity = 1;
  remote.event_log_capacity = 16;
  remote.shared_sequencer_offset = 192;

  dsidle::ConfigureLatencySimulatorForPool(pool, config, 0);
  auto* word = reinterpret_cast<std::uint64_t*>(
      static_cast<std::byte*>(pool.hwcc_base()) + 8192);
  latency_sim::CountedMemoryStore(latency_sim::PoolKind::kHwcc, word,
                                  std::uint64_t{7});

  const auto* layout_meta = pool.static_layout();
  const auto* remote_header = reinterpret_cast<const dsidle::RemoteInstrumentationHeader*>(
      static_cast<const std::byte*>(pool.base()) + layout_meta->diagnostic_offset);
  void* sequence = static_cast<std::byte*>(pool.base()) +
                   layout_meta->diagnostic_offset + remote_header->sequence_offset;
  void* event_log = static_cast<std::byte*>(pool.base()) +
                    remote_header->event_log_offset;
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    // A real worker process starts with no inherited simulator attachment.  A
    // forked canary must clear the parent's process-local pointer before its
    // bootstrap AttachAt validation, or that validation would be logged into
    // the parent's shared sequence.
    latency_sim::GlobalLatencySimulator().Configure({});
    auto attached = dsidle::SharedPool::AttachAt(
        path, layout.total_bytes, reinterpret_cast<void*>(0x500000000000ULL));
    dsidle::ConfigureLatencySimulatorForPool(attached, config, 1);
    auto* child_word = reinterpret_cast<std::uint64_t*>(
        static_cast<std::byte*>(attached.hwcc_base()) + 8192);
    assert(latency_sim::CountedMemoryLoad(
               latency_sim::PoolKind::kHwcc, child_word) == 7);
    const auto child_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(child_stats.remote_events == 2);
    attached.Close();
    _exit(0);
  }
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
         WEXITSTATUS(status) == 0);
  const auto stats = latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
  assert(stats.remote_events == 2);
  assert(stats.remote_dirty_handoffs == 1);
  assert(reinterpret_cast<std::atomic<std::uint64_t>*>(sequence)
             ->load(std::memory_order_acquire) == 2);
  latency_sim::GlobalLatencySimulator().DetachSharedRemoteLog(sequence, event_log);
  latency_sim::GlobalLatencySimulator().Configure({});
  pool.Close();
  assert(unlink(path) == 0);
}

}  // namespace

int main() {
  TestRemoteInstrumentationAcrossMappings();
  char path[] = "/tmp/dsidle-pool-test.XXXXXX";
  const int placeholder = mkstemp(path);
  assert(placeholder >= 0);
  close(placeholder);
  unlink(path);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 32ULL << 20,
                                  32ULL << 20, 96ULL << 20};
  auto pool = dsidle::SharedPool::Create(path, layout);
  assert(pool.header()->magic == dsidle::kPoolMagic);
  assert(pool.root_control()->version.load() == 0);
  bool rejected_initializing = false;
  try {
    auto premature = dsidle::SharedPool::Attach(path, layout);
  } catch (const std::runtime_error&) {
    rejected_initializing = true;
  }
  assert(rejected_initializing);
  dsidle::InitializePoolMetadata(pool, {1, 1, 16});
  bool rejected_incomplete = false;
  try {
    dsidle::FinalizePoolInitialization(pool);
  } catch (const std::runtime_error&) {
    rejected_incomplete = true;
  }
  assert(rejected_incomplete);
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 1);
  dsidle::FinalizePoolInitialization(pool);
  assert(pool.header()->state.load(std::memory_order_acquire) ==
         static_cast<std::uint64_t>(dsidle::PoolState::kReady));
  bool rejected_wrong_layout = false;
  try {
    auto wrong = dsidle::SharedPool::Attach(
        path, {layout.total_bytes, 0, 64ULL << 20,
               64ULL << 20, 64ULL << 20});
  } catch (const std::runtime_error&) {
    rejected_wrong_layout = true;
  }
  assert(rejected_wrong_layout);
  const auto current_abi = pool.header()->abi_version;
  pool.header()->abi_version = current_abi - 1;
  bool rejected_stale_abi = false;
  try {
    auto stale = dsidle::SharedPool::Attach(path, layout);
  } catch (const std::runtime_error&) {
    rejected_stale_abi = true;
  }
  assert(rejected_stale_abi);
  pool.header()->abi_version = current_abi;
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    auto attached = dsidle::SharedPool::Attach(path, layout);
    std::memcpy(attached.swcc_base(), "shared", 7);
    attached.root_control()->version.store(7, std::memory_order_release);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(std::strcmp(static_cast<const char*>(pool.swcc_base()), "shared") == 0);
  assert(pool.root_control()->version.load(std::memory_order_acquire) == 7);
  pool.Close();
  assert(unlink(path) == 0);
  std::cout << "shared pool attach contract OK\n";
}
