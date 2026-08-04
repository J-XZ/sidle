#include "dsidle/latency_simulator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {

constexpr size_t kPage = 4096;

latency_sim::Config FixedConfig(double swcc_ns, double hwcc_ns) {
  latency_sim::Config config;
  config.fixed_latency.enabled = true;
  config.fixed_latency.cache_line_bytes = 64;
  config.fixed_latency.swcc_fixed_ns_per_line = swcc_ns;
  config.fixed_latency.hwcc_fixed_ns_per_line = hwcc_ns;
  config.fixed_latency.foreground_enabled = true;
  config.fixed_latency.background_enabled = true;
  return config;
}

struct TestBuffers {
  explicit TestBuffers(latency_sim::LatencySimulator* simulator) {
    swcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    hwcc = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(swcc != MAP_FAILED && hwcc != MAP_FAILED);
    simulator->RegisterPool(latency_sim::PoolKind::kSwcc, swcc, kPage);
    simulator->RegisterPool(latency_sim::PoolKind::kHwcc, hwcc, kPage);
  }
  ~TestBuffers() {
    munmap(swcc, kPage);
    munmap(hwcc, kPage);
  }
  void* swcc = nullptr;
  void* hwcc = nullptr;
};

void ExpectInvalid(latency_sim::Config config) {
  bool rejected = false;
  try {
    latency_sim::LatencySimulator simulator(config);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);
}

void ExpectDeath(latency_sim::LatencySimulator* simulator,
                 void (*operation)(latency_sim::LatencySimulator*,
                                   TestBuffers*),
                 TestBuffers* buffers) {
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    operation(simulator, buffers);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status));
}

void DeathOutOfRange(latency_sim::LatencySimulator* simulator,
                     TestBuffers* buffers) {
  simulator->RecordRange(
      latency_sim::PoolKind::kHwcc, latency_sim::AccessKind::kRead,
      static_cast<std::byte*>(buffers->hwcc) + kPage - 1, 2);
}
void DeathWrongDomain(latency_sim::LatencySimulator* simulator,
                      TestBuffers* buffers) {
  simulator->RecordRange(latency_sim::PoolKind::kSwcc,
                         latency_sim::AccessKind::kRead, buffers->hwcc, 64);
}
void DeathNoScope(latency_sim::LatencySimulator* simulator,
                  TestBuffers* buffers) {
  simulator->RecordRange(latency_sim::PoolKind::kHwcc,
                         latency_sim::AccessKind::kRead, buffers->hwcc, 64);
}
void DeathMultiplyOverflow(latency_sim::LatencySimulator* simulator,
                           TestBuffers* buffers) {
  simulator->RecordRange(latency_sim::PoolKind::kSwcc,
                         latency_sim::AccessKind::kRead, buffers->swcc, 128);
}
void DeathAddOverflow(latency_sim::LatencySimulator* simulator,
                      TestBuffers* buffers) {
  simulator->RecordRange(latency_sim::PoolKind::kSwcc,
                         latency_sim::AccessKind::kRead, buffers->swcc, 64);
  simulator->RecordRange(latency_sim::PoolKind::kSwcc,
                         latency_sim::AccessKind::kRead, buffers->swcc, 64);
}

void TestDisabledFastPath() {
  latency_sim::LatencySimulator simulator({});
  const size_t tls_before = simulator.ThreadStateCountForTest();
  TestBuffers buffers(&simulator);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  simulator.RecordAtomicAccess(latency_sim::AtomicDomain::kHwcc,
                               latency_sim::AccessKind::kAtomicLoad,
                               buffers.hwcc, sizeof(std::uint64_t));
  simulator.EndScopeAndDelay();
  assert(simulator.feature_mask() == 0);
  assert(simulator.PendingDelayNsForTest() == 0);
  assert(simulator.ThreadStateCountForTest() == tls_before);
}

void TestValidation() {
  auto invalid = FixedConfig(1, 2);
  invalid.fixed_latency.cache_line_bytes = 0;
  ExpectInvalid(invalid);
  invalid = FixedConfig(1, 2);
  invalid.fixed_latency.cache_line_bytes = 3;
  ExpectInvalid(invalid);
  invalid = FixedConfig(-1, 2);
  ExpectInvalid(invalid);
  invalid = FixedConfig(1, std::numeric_limits<double>::quiet_NaN());
  ExpectInvalid(invalid);
}

void TestLineCoverageAndScopes() {
  latency_sim::LatencySimulator simulator(FixedConfig(3, 7));
  TestBuffers buffers(&simulator);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 0);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kWrite,
                        static_cast<std::byte*>(buffers.swcc) + 1, 64);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte*>(buffers.hwcc) + 1, 64);
  assert(simulator.PendingDelayNsForTest() == 26);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  assert(simulator.PendingDelayNsForTest() == 33);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 33);
  simulator.EndScopeAndDelay();
  assert(simulator.PendingDelayNsForTest() == 0);

  simulator.Configure(FixedConfig(5, 11));
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  simulator.RecordRange(latency_sim::PoolKind::kOwnerPrivateSwcc,
                        latency_sim::AccessKind::kWrite, buffers.swcc, 128);
  assert(simulator.PendingDelayNsForTest() == 10);
  simulator.EndScopeAndDelay();
}

void TestScopeGatesAndWorkerIsolation() {
  latency_sim::LatencySimulator simulator(FixedConfig(3, 7));
  TestBuffers buffers(&simulator);
  auto config = FixedConfig(3, 7);
  config.fixed_latency.background_enabled = false;
  simulator.Configure(config);
  simulator.BeginScope(latency_sim::ScopeKind::kMerge);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  assert(simulator.PendingDelayNsForTest() == 0);
  simulator.EndScopeAndDelay();

  config.fixed_latency.background_enabled = true;
  simulator.Configure(config);
  std::atomic<std::uint64_t> worker_pending{0};
  std::thread worker([&] {
    simulator.BeginScope(latency_sim::ScopeKind::kMerge);
    simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                          latency_sim::AccessKind::kRead, buffers.hwcc, 64);
    worker_pending.store(simulator.PendingDelayNsForTest(),
                         std::memory_order_release);
    simulator.EndScopeAndDelay();
  });
  worker.join();
  assert(worker_pending.load(std::memory_order_acquire) == 7);
  assert(simulator.PendingDelayNsForTest() == 0);
}

void TestAtomicSemantics() {
  // The typed wrappers charge through the process-global simulator.
  auto& simulator = latency_sim::GlobalLatencySimulator();
  simulator.Configure(FixedConfig(3, 7));
  TestBuffers buffers(&simulator);
  auto* value = new (buffers.hwcc) std::atomic<std::uint64_t>{7};
  auto* private_value = new (buffers.swcc) std::atomic<std::uint64_t>{11};
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    assert(latency_sim::FixedLatencyAtomicLoad(
               *value, std::memory_order_acquire,
               latency_sim::AtomicDomain::kHwcc) == 7);
    latency_sim::FixedLatencyAtomicExchange(
        *value, std::uint64_t{9}, std::memory_order_acq_rel,
        latency_sim::AtomicDomain::kHwcc);
    latency_sim::FixedLatencyAtomicFetchAdd(
        *value, std::uint64_t{1}, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kHwcc);
    std::uint64_t expected = 10;
    assert(latency_sim::FixedLatencyAtomicCompareExchangeStrong(
        *value, expected, std::uint64_t{11}, std::memory_order_acq_rel,
        std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
    expected = 10;
    assert(!latency_sim::FixedLatencyAtomicCompareExchangeWeak(
        *value, expected, std::uint64_t{12}, std::memory_order_acq_rel,
        std::memory_order_acquire, latency_sim::AtomicDomain::kHwcc));
    assert(expected == 11);
    latency_sim::FixedLatencyAtomicFetchOr(
        *private_value, std::uint64_t{1}, std::memory_order_relaxed,
        latency_sim::AtomicDomain::kOwnerPrivateSwcc);
    assert(latency_sim::FixedLatencyAtomicFetchAnd(
               *private_value, ~std::uint64_t{1}, std::memory_order_relaxed,
               latency_sim::AtomicDomain::kOwnerPrivateSwcc) == 11);
    // Local DRAM atomics do not go through the wrapper.
    assert(value->fetch_xor(std::uint64_t{1}, std::memory_order_relaxed) == 11);
    assert(simulator.PendingDelayNsForTest() == 7 * 5 + 3 * 2);
  }
  simulator.Configure({});
}

void TestFractionalAndOverflow() {
  latency_sim::LatencySimulator simulator(FixedConfig(0.4, 0.2));
  TestBuffers buffers(&simulator);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64 * 5);
  assert(simulator.PendingDelayPsForTest() == 2000);
  assert(simulator.PendingDelayNsForTest() == 2);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte*>(buffers.swcc) + 64, 64);
  simulator.RecordRange(latency_sim::PoolKind::kHwcc,
                        latency_sim::AccessKind::kRead, buffers.hwcc, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead,
                        static_cast<std::byte*>(buffers.swcc) + 128, 64);
  assert(simulator.PendingDelayPsForTest() == 3000);
  assert(simulator.PendingDelayNsForTest() == 3);
  simulator.EndScopeAndDelay();

  simulator.Configure(FixedConfig(1.25, 0.0));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64 * 4);
  assert(simulator.PendingDelayPsForTest() == 5000);
  assert(simulator.PendingDelayNsForTest() == 5);
  simulator.EndScopeAndDelay();

  // Flush/invalidate audit labels never add a second delay.
  simulator.Configure(FixedConfig(7, 13));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kFlush, buffers.swcc, 64);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kInvalidate, buffers.swcc, 64);
  assert(simulator.PendingDelayNsForTest() == 7);
  simulator.EndScopeAndDelay();

  // Maximum legal value accumulates exactly; overflow hard fails.
  simulator.Configure(FixedConfig(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  simulator.RecordRange(latency_sim::PoolKind::kSwcc,
                        latency_sim::AccessKind::kRead, buffers.swcc, 64);
  assert(simulator.PendingDelayPsForTest() == 10000000000000000000ull);
  simulator.Configure({});
  simulator.EndScopeAndDelay();

  simulator.Configure(FixedConfig(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  ExpectDeath(&simulator, DeathMultiplyOverflow, &buffers);
  simulator.Configure({});
  simulator.EndScopeAndDelay();

  simulator.Configure(FixedConfig(1.0e16, 0.0));
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  ExpectDeath(&simulator, DeathAddOverflow, &buffers);
  simulator.Configure({});
  simulator.EndScopeAndDelay();
}

void TestHardFailures() {
  latency_sim::LatencySimulator simulator(FixedConfig(3, 7));
  TestBuffers buffers(&simulator);
  ExpectDeath(&simulator, DeathNoScope, &buffers);
  simulator.BeginScope(latency_sim::ScopeKind::kForeground);
  ExpectDeath(&simulator, DeathWrongDomain, &buffers);
  ExpectDeath(&simulator, DeathOutOfRange, &buffers);
  simulator.EndScopeAndDelay();
  simulator.Configure({});
}

void TestRoundingAndTickConversion() {
  // Pure conversions never busy-wait, so extreme values are safe to test.
  assert(latency_sim::RoundDelayPsToNsForTest(0) == 0);
  assert(latency_sim::RoundDelayPsToNsForTest(499) == 0);
  assert(latency_sim::RoundDelayPsToNsForTest(500) == 1);
  assert(latency_sim::RoundDelayPsToNsForTest(999) == 1);
  assert(latency_sim::RoundDelayPsToNsForTest(1000) == 1);
  assert(latency_sim::RoundDelayPsToNsForTest(1001) == 1);
  assert(latency_sim::RoundDelayPsToNsForTest(1500) == 2);
  assert(latency_sim::RoundDelayPsToNsForTest(
             std::numeric_limits<uint64_t>::max()) == 18446744073709552ull);
  assert(latency_sim::RoundDelayPsToNsForTest(
             std::numeric_limits<uint64_t>::max() - 114) ==
         18446744073709552ull);
  assert(latency_sim::RoundDelayPsToNsForTest(
             std::numeric_limits<uint64_t>::max() - 499) ==
         18446744073709551ull);

  assert(latency_sim::TicksForDelayNsForTest(3.0, 0) == 0);
  assert(latency_sim::TicksForDelayNsForTest(3.0, 1) == 3);
  assert(latency_sim::TicksForDelayNsForTest(3.5, 1) == 4);
  assert(latency_sim::TicksForDelayNsForTest(0.5, 3) == 2);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    latency_sim::TicksForDelayNsForTest(1e300, 1000);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status));
  const pid_t child2 = fork();
  assert(child2 >= 0);
  if (child2 == 0) {
    latency_sim::TicksForDelayNsForTest(0.0, 1);
    _exit(0);
  }
  assert(waitpid(child2, &status, 0) == child2);
  assert(WIFSIGNALED(status));
}

void TestPoolReopenLifecycle() {
  // Sequential open/close of two different mappings in one process.  The
  // re-enable boundary (ConfigureLatencySimulatorForPool) disables the gate
  // and clears stale registrations before registering the new mapping, so the
  // old mapping's addresses are rejected and the new mapping charges normally.
  auto& global = latency_sim::GlobalLatencySimulator();
  global.Configure(latency_sim::Config{});
  global.ClearPoolRegistrations();
  void* gen1 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* gen2 = mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(gen1 != MAP_FAILED && gen2 != MAP_FAILED);
  global.RegisterPool(latency_sim::PoolKind::kHwcc, gen1, kPage);
  global.Configure(FixedConfig(1, 1));
  global.BeginScope(latency_sim::ScopeKind::kForeground);
  global.RecordRange(latency_sim::PoolKind::kHwcc,
                     latency_sim::AccessKind::kRead, gen1, 64);
  assert(global.PendingDelayNsForTest() == 1);
  global.EndScopeAndDelay();

  global.Configure(latency_sim::Config{});
  global.ClearPoolRegistrations();
  global.RegisterPool(latency_sim::PoolKind::kHwcc, gen2, kPage);
  global.Configure(FixedConfig(1, 1));
  global.BeginScope(latency_sim::ScopeKind::kForeground);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    global.RecordRange(latency_sim::PoolKind::kHwcc,
                       latency_sim::AccessKind::kRead, gen1, 64);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status));
  global.RecordRange(latency_sim::PoolKind::kHwcc,
                     latency_sim::AccessKind::kRead, gen2, 64);
  assert(global.PendingDelayNsForTest() == 1);
  global.EndScopeAndDelay();
  global.Configure(latency_sim::Config{});
  global.ClearPoolRegistrations();
  munmap(gen1, kPage);
  munmap(gen2, kPage);
}

void TestTscSpin() {
  if (!latency_sim::TscSpinAvailableForTest()) return;
  const auto begin = std::chrono::steady_clock::now();
  latency_sim::DelaySpinNsForTest(1000);
  const auto end = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      end - begin);
  assert(elapsed.count() >= 500);
}

}  // namespace

int main() {
  TestDisabledFastPath();
  TestValidation();
  TestLineCoverageAndScopes();
  TestScopeGatesAndWorkerIsolation();
  TestAtomicSemantics();
  TestFractionalAndOverflow();
  TestHardFailures();
  TestRoundingAndTickConversion();
  TestPoolReopenLifecycle();
  TestTscSpin();
  return 0;
}
