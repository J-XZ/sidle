#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"
#include "dsidle/latency_simulator.h"
#include "third_party/masstree-beta/dsidle_value_access.hh"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void TestLatencyReopenLifecycle() {
  char path_a[] = "/tmp/dsidle-pool-reopen-a.XXXXXX";
  const int fd_a = mkstemp(path_a);
  assert(fd_a >= 0);
  close(fd_a);
  unlink(path_a);
  char path_b[] = "/tmp/dsidle-pool-reopen-b.XXXXXX";
  const int fd_b = mkstemp(path_b);
  assert(fd_b >= 0);
  close(fd_b);
  unlink(path_b);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 32ULL << 20,
                                  32ULL << 20, 96ULL << 20};
  latency_sim::Config enabled;
  enabled.fixed_latency.enabled = true;
  enabled.fixed_latency.cache_line_bytes = 64;
  enabled.fixed_latency.swcc_fixed_ns_per_line = 1;
  enabled.fixed_latency.hwcc_fixed_ns_per_line = 1;
  void* old_hwcc = nullptr;
  auto& simulator = latency_sim::GlobalLatencySimulator();
  auto pool_a = dsidle::SharedPool::Create(path_a, layout);
  dsidle::ConfigureLatencySimulatorForPool(pool_a, enabled, 0);
  old_hwcc = pool_a.hwcc_base();
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kOther);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, old_hwcc, 64);
    assert(simulator.PendingDelayNsForTest() > 0);
  }
  // Reopen with a different backing file.  Pool bring-up must happen at an
  // explicit gate-disabled boundary (the same boundary ConfigureLatencySimulator
  // enforces before registration), then ConfigureLatencySimulatorForPool clears
  // the old registration so the old mapping's address is rejected.
  simulator.Configure({});
  auto pool_b = dsidle::SharedPool::Create(path_b, layout);
  // Keep both mappings alive so the kernel cannot reuse pool_a's VA for
  // pool_b; the rejection below must be a registration decision, not an
  // address coincidence.
  assert(pool_b.hwcc_base() != old_hwcc);
  dsidle::ConfigureLatencySimulatorForPool(pool_b, enabled, 0);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kOther);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                            latency_sim::AccessKind::kRead, old_hwcc, 64);
      _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, pool_b.hwcc_base(),
                          64);
    assert(simulator.PendingDelayNsForTest() > 0);
  }
  simulator.Configure({});
  simulator.ClearPoolRegistrations();
  unlink(path_a);
  unlink(path_b);
}

latency_sim::Config ClassifierEnabledConfig(double swcc_ns, double hwcc_ns) {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.cache_line_bytes = 64;
  config.fixed_latency.swcc_fixed_ns_per_line = swcc_ns;
  config.fixed_latency.hwcc_fixed_ns_per_line = hwcc_ns;
  config.fixed_latency.foreground_enabled = true;
  config.fixed_latency.background_enabled = true;
  return config;
}

// The SWCC value-range classifier must depend only on the immutable
// thread-local cache published by the explicit binding stage: binding inside
// a scope charges the real HWCC layout reads, remapping replaces the cached
// range so old mapping addresses are no longer classified as SWCC, and an
// enabled thread without a binding hard fails instead of silently treating a
// potential SWCC access as local DRAM.
void TestSwccClassifierBindingAndRemap() {
  char path_a[] = "/tmp/dsidle-classifier-a.XXXXXX";
  const int fd_a = mkstemp(path_a);
  assert(fd_a >= 0);
  close(fd_a);
  unlink(path_a);
  char path_b[] = "/tmp/dsidle-classifier-b.XXXXXX";
  const int fd_b = mkstemp(path_b);
  assert(fd_b >= 0);
  close(fd_b);
  unlink(path_b);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 32ULL << 20,
                                  32ULL << 20, 96ULL << 20};
  auto& simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure({});
  simulator.ClearPoolRegistrations();
  auto pool_a = dsidle::SharedPool::Create(path_a, layout);
  dsidle::InitializePoolMetadata(pool_a, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool_a, 2);
  dsidle::FinalizePoolInitialization(pool_a);

  // Enabled binding without a scope must hard fail (the wrapped HWCC layout
  // read charges outside any scope).
  dsidle::ConfigureLatencySimulatorForPool(pool_a, ClassifierEnabledConfig(1, 2), 0);
  {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      dsidle::ConfigureCurrentSwccAllocator(pool_a, 2, 0);
      _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
  }
  // Enabled binding inside a short scope charges the three real HWCC layout
  // reads (shard_count, swcc_offset, swcc_bytes) once each.
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    dsidle::ConfigureCurrentSwccAllocator(pool_a, 2, 0);
    assert(simulator.PendingDelayNsForTest() == 6);
  }
  // Classifier reads only the cached range: SWCC addresses are observed,
  // HWCC and local DRAM addresses are not, and the pool header is never
  // dereferenced (the audit script enforces the source-level rule).
  assert(dsidle_masstree::IsObservedSwccRange(pool_a.swcc_base(), 64));
  assert(!dsidle_masstree::IsObservedSwccRange(pool_a.hwcc_base(), 64));
  void* local = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(local != MAP_FAILED);
  assert(!dsidle_masstree::IsObservedSwccRange(local, 64));
  // A thread that never bound must hard fail while fixed latency is enabled.
  bool unbound_threw = false;
  std::thread unbound([&] {
    try {
      (void)dsidle_masstree::IsObservedSwccRange(pool_a.swcc_base(), 64);
    } catch (const std::runtime_error&) {
      unbound_threw = true;
    }
  });
  unbound.join();
  assert(unbound_threw);

  // Sequential remap: pool B stays alive together with pool A so the kernel
  // cannot reuse A's mapping address.  Re-binding replaces the cached range:
  // A's old SWCC range is no longer classified as SWCC, B's is.
  // Pool bring-up (SharedPool::Create) runs only at a gate-disabled boundary;
  // its wrapped metadata stores would otherwise hard fail.
  simulator.Configure({});
  auto pool_b = dsidle::SharedPool::Create(path_b, layout);
  dsidle::InitializePoolMetadata(pool_b, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool_b, 2);
  dsidle::FinalizePoolInitialization(pool_b);
  dsidle::ConfigureLatencySimulatorForPool(pool_b, ClassifierEnabledConfig(1, 2), 0);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    dsidle::ConfigureCurrentSwccAllocator(pool_b, 2, 0);
  }
  assert(!dsidle_masstree::IsObservedSwccRange(pool_a.swcc_base(), 64));
  assert(dsidle_masstree::IsObservedSwccRange(pool_b.swcc_base(), 64));
  // Closing the bound pool clears this thread's cache; the classifier then
  // refuses to guess.
  pool_b.Close();
  bool after_close_threw = false;
  try {
    (void)dsidle_masstree::IsObservedSwccRange(pool_a.swcc_base(), 64);
  } catch (const std::runtime_error&) {
    after_close_threw = true;
  }
  assert(after_close_threw);
  munmap(local, 4096);
  simulator.Configure({});
  simulator.ClearPoolRegistrations();
  pool_a.Close();
  unlink(path_a);
  unlink(path_b);
}

// Boundary behaviour of the SWCC value-range classifier: fully-inside and
// fully-outside ranges keep their boolean semantics, ranges that partially
// straddle either SWCC boundary and uintptr_t-overflowing ranges hard fail
// instead of being silently treated as local DRAM, and nullptr/zero-length
// keep the established non-observed semantics.
void TestSwccClassifierBoundaries() {
  char path[] = "/tmp/dsidle-classifier-bounds.XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 32ULL << 20,
                                  32ULL << 20, 96ULL << 20};
  auto& simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure({});
  simulator.ClearPoolRegistrations();
  auto pool = dsidle::SharedPool::Create(path, layout);
  dsidle::InitializePoolMetadata(pool, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 2);
  dsidle::FinalizePoolInitialization(pool);
  dsidle::ConfigureLatencySimulatorForPool(pool, ClassifierEnabledConfig(1, 2), 0);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    dsidle::ConfigureCurrentSwccAllocator(pool, 2, 0);
  }
  const auto swcc_begin =
      reinterpret_cast<std::uintptr_t>(pool.swcc_base());
  const auto swcc_end =
      swcc_begin + pool.header()->swcc_bytes;
  // Fully inside.
  assert(dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_begin), 64));
  assert(dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_end - 64), 64));
  assert(dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_begin), swcc_end - swcc_begin));
  // Fully outside on either side.
  assert(!dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_begin - 4096), 64));
  assert(!dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_end), 64));
  // nullptr and zero length keep their current semantics.
  assert(!dsidle_masstree::IsObservedSwccRange(nullptr, 64));
  assert(!dsidle_masstree::IsObservedSwccRange(
      reinterpret_cast<const void*>(swcc_begin), 0));
  // Partial straddle across either boundary hard fails.
  bool left_straddle_threw = false;
  try {
    (void)dsidle_masstree::IsObservedSwccRange(
        reinterpret_cast<const void*>(swcc_begin - 32), 64);
  } catch (const std::runtime_error&) {
    left_straddle_threw = true;
  }
  assert(left_straddle_threw);
  bool right_straddle_threw = false;
  try {
    (void)dsidle_masstree::IsObservedSwccRange(
        reinterpret_cast<const void*>(swcc_end - 32), 64);
  } catch (const std::runtime_error&) {
    right_straddle_threw = true;
  }
  assert(right_straddle_threw);
  // uintptr_t overflow hard fails while fixed latency is enabled.
  bool overflow_threw = false;
  try {
    (void)dsidle_masstree::IsObservedSwccRange(
        reinterpret_cast<const void*>(std::numeric_limits<std::uintptr_t>::max() - 7),
        64);
  } catch (const std::runtime_error&) {
    overflow_threw = true;
  }
  assert(overflow_threw);
  simulator.Configure({});
  simulator.ClearPoolRegistrations();
  pool.Close();
  unlink(path);
}

// SharedPool moves must migrate the current thread's SWCC binding (pool
// pointer, SharedPoolBase and range cache) from the source to the
// destination, close the destination's previous mapping first, and leave the
// moved-from shell unable to clear the moved-to binding.
void TestSharedPoolMoveBinding() {
  char path_a[] = "/tmp/dsidle-move-a.XXXXXX";
  const int fd_a = mkstemp(path_a);
  assert(fd_a >= 0);
  close(fd_a);
  unlink(path_a);
  char path_b[] = "/tmp/dsidle-move-b.XXXXXX";
  const int fd_b = mkstemp(path_b);
  assert(fd_b >= 0);
  close(fd_b);
  unlink(path_b);
  char path_c[] = "/tmp/dsidle-move-c.XXXXXX";
  const int fd_c = mkstemp(path_c);
  assert(fd_c >= 0);
  close(fd_c);
  unlink(path_c);
  const dsidle::PoolLayout layout{128ULL << 20, 0, 32ULL << 20,
                                  32ULL << 20, 96ULL << 20};
  auto& simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure({});
  simulator.ClearPoolRegistrations();

  // Move construction migrates the binding.
  auto pool = dsidle::SharedPool::Create(path_a, layout);
  dsidle::InitializePoolMetadata(pool, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 2);
  dsidle::FinalizePoolInitialization(pool);
  dsidle::ConfigureLatencySimulatorForPool(pool, ClassifierEnabledConfig(1, 2), 0);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    dsidle::ConfigureCurrentSwccAllocator(pool, 2, 1);
    assert(simulator.PendingDelayNsForTest() == 6);
  }
  assert(dsidle::CurrentSharedPool().base() == pool.base());
  assert(dsidle::CurrentSwccShard() == 1);
  assert(dsidle_masstree::IsObservedSwccRange(pool.swcc_base(), 64));
  auto moved = std::move(pool);
  assert(dsidle::CurrentSharedPool().base() == moved.base());
  assert(dsidle::CurrentSwccShard() == 1);
  assert(dsidle_masstree::IsObservedSwccRange(moved.swcc_base(), 64));
  // Moved-from destructor/Close must not break the moved-to binding.
  pool.Close();
  assert(dsidle::CurrentSharedPool().base() == moved.base());
  assert(dsidle_masstree::IsObservedSwccRange(moved.swcc_base(), 64));
  // Moved-to Close invalidates all current-thread TLS state.
  const auto moved_swcc =
      reinterpret_cast<std::uintptr_t>(moved.swcc_base());
  moved.Close();
  bool shared_pool_threw = false;
  try {
    (void)dsidle::CurrentSharedPool();
  } catch (const std::runtime_error&) {
    shared_pool_threw = true;
  }
  assert(shared_pool_threw);
  bool base_threw = false;
  try {
    (void)dsidle::SharedPoolBase();
  } catch (const std::runtime_error&) {
    base_threw = true;
  }
  assert(base_threw);
  bool classifier_threw = false;
  try {
    (void)dsidle_masstree::IsObservedSwccRange(
        reinterpret_cast<const void*>(moved_swcc), 64);
  } catch (const std::runtime_error&) {
    classifier_threw = true;
  }
  assert(classifier_threw);

  // Move assignment: destination's old mapping is closed first, and the
  // source's binding is migrated to the destination.
  simulator.Configure({});
  auto dest = dsidle::SharedPool::Create(path_b, layout);
  dsidle::InitializePoolMetadata(dest, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(dest, 2);
  dsidle::FinalizePoolInitialization(dest);
  auto src = dsidle::SharedPool::Create(path_c, layout);
  dsidle::InitializePoolMetadata(src, {2, 2, 64});
  dsidle::FixedBlockShardAllocator::InitializeAll(src, 2);
  dsidle::FinalizePoolInitialization(src);
  const auto old_dest_base = dest.base();
  const auto src_base = src.base();
  assert(old_dest_base != src_base);
  dsidle::ConfigureLatencySimulatorForPool(src, ClassifierEnabledConfig(1, 2), 0);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    dsidle::ConfigureCurrentSwccAllocator(src, 2, 0);
  }
  assert(dsidle::CurrentSharedPool().base() == src_base);
  dest = std::move(src);
  // Destination no longer owns its old mapping and the binding now points at
  // the moved-to destination.
  assert(dest.base() == src_base && dest.base() != old_dest_base);
  assert(dsidle::CurrentSharedPool().base() == dest.base());
  assert(dsidle::CurrentSwccShard() == 0);
  assert(dsidle_masstree::IsObservedSwccRange(dest.swcc_base(), 64));
  // Moved-from shell must not clear the moved-to binding.
  src.Close();
  assert(dsidle::CurrentSharedPool().base() == dest.base());
  // Disabled fast path: classifier returns before TLS work and needs no
  // binding; binding outside a scope performs no charged work.
  const size_t tls_before = simulator.ThreadStateCountForTest();
  simulator.Configure({});
  assert(!dsidle_masstree::IsObservedSwccRange(dest.swcc_base(), 64));
  dsidle::ConfigureCurrentSwccAllocator(dest, 2, 0);
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(simulator.ThreadStateCountForTest() == tls_before);
  dest.Close();
  simulator.ClearPoolRegistrations();
  unlink(path_a);
  unlink(path_b);
  unlink(path_c);
}

}  // namespace

int main() {
  TestLatencyReopenLifecycle();
  TestSwccClassifierBindingAndRemap();
  TestSwccClassifierBoundaries();
  TestSharedPoolMoveBinding();
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
