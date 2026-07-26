#include "dsidle/shard_allocator.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
void TestConcurrentAbaIsRejected() {
  constexpr std::uint64_t a = 64;
  constexpr std::uint64_t b = 128;
  std::atomic<std::uint64_t> head{
      dsidle::TaggedFreeListHead::Encode(a, 7)};
  std::atomic<unsigned> phase{0};
  std::atomic<bool> stale_cas_succeeded{true};

  std::thread stale_reader([&] {
    auto stale = head.load(std::memory_order_acquire);
    assert(dsidle::TaggedFreeListHead::Offset(stale) == a);
    // This is the next offset that was observed with the stale A head.
    constexpr auto stale_next = b;
    phase.store(1, std::memory_order_release);
    while (phase.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    const auto desired =
        dsidle::TaggedFreeListHead::Advance(stale, stale_next);
    stale_cas_succeeded.store(
        head.compare_exchange_strong(stale, desired,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire),
        std::memory_order_release);
  });

  std::thread aba_writer([&] {
    while (phase.load(std::memory_order_acquire) != 1)
      std::this_thread::yield();
    auto current = head.load(std::memory_order_acquire);
    assert(dsidle::TaggedFreeListHead::Offset(current) == a);
    assert(head.compare_exchange_strong(
        current, dsidle::TaggedFreeListHead::Advance(current, b),
        std::memory_order_acq_rel, std::memory_order_acquire));
    current = head.load(std::memory_order_acquire);
    assert(dsidle::TaggedFreeListHead::Offset(current) == b);
    assert(head.compare_exchange_strong(
        current, dsidle::TaggedFreeListHead::Advance(current, 0),
        std::memory_order_acq_rel, std::memory_order_acquire));
    current = head.load(std::memory_order_acquire);
    assert(dsidle::TaggedFreeListHead::Offset(current) == 0);
    assert(head.compare_exchange_strong(
        current, dsidle::TaggedFreeListHead::Advance(current, a),
        std::memory_order_acq_rel, std::memory_order_acquire));
    phase.store(2, std::memory_order_release);
  });

  stale_reader.join();
  aba_writer.join();
  assert(!stale_cas_succeeded.load(std::memory_order_acquire));
  assert(dsidle::TaggedFreeListHead::Offset(
             head.load(std::memory_order_acquire)) == a);
  assert(dsidle::TaggedFreeListHead::Tag(
             head.load(std::memory_order_acquire)) == 10);
  bool rejected_wrap = false;
  try {
    dsidle::TaggedFreeListHead::Advance(
        dsidle::TaggedFreeListHead::Encode(
            a, dsidle::TaggedFreeListHead::kMaximumTag),
        b);
  } catch (const std::runtime_error&) {
    rejected_wrap = true;
  }
  assert(rejected_wrap);
}
}  // namespace

int main() {
  TestConcurrentAbaIsRejected();
  char path[] = "/tmp/dsidle-shard-test.XXXXXX";
  int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  constexpr std::uint64_t kPoolBytes = 128ULL << 20;
  auto pool = dsidle::SharedPool::Create(path, {kPoolBytes, 0, 32ULL<<20, 32ULL<<20, 96ULL<<20});
  dsidle::InitializePoolMetadata(pool, {2, 2, 64});
  dsidle::NodeControlSlab controls(pool);
  const auto node = controls.Reserve(2ULL << 20, 7);
  controls.Publish(node, 0);
  assert(node && node.get(pool.base())->generation == 1);
  controls.Retire(node, 1);
  controls.Release(node);
  const auto reused_node = controls.Reserve((2ULL << 20) + 64, 8);
  controls.Publish(reused_node, 0);
  assert(reused_node == node && reused_node.get(pool.base())->generation == 2);
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 2);
  dsidle::FinalizePoolInitialization(pool);
  dsidle::FixedBlockShardAllocator alloc(pool, 2, 64);
  const auto first = alloc.Allocate(0); assert(first);
  const auto pid = fork(); assert(pid >= 0);
  if (pid == 0) {
    auto attached = dsidle::SharedPool::Attach(
        path, {kPoolBytes, 0, 32ULL << 20, 32ULL << 20, 96ULL << 20});
    dsidle::FixedBlockShardAllocator child_alloc(attached, 2, 64);
    child_alloc.Free(0, first, 9);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(alloc.HarvestRemote(0) == 1);
  const auto reused = alloc.Allocate(0); assert(reused == first);
  std::mutex live_mutex;
  std::unordered_set<std::uint64_t> live_offsets;
  std::vector<std::thread> allocator_threads;
  for (unsigned thread_index = 0; thread_index < 8; ++thread_index) {
    allocator_threads.emplace_back([&, thread_index] {
      for (unsigned iteration = 0; iteration < 2'000; ++iteration) {
        const auto block = alloc.Allocate(0);
        {
          std::lock_guard<std::mutex> lock(live_mutex);
          assert(live_offsets.insert(block.value()).second);
        }
        if ((iteration + thread_index) % 7 == 0)
          std::this_thread::yield();
        {
          std::lock_guard<std::mutex> lock(live_mutex);
          assert(live_offsets.erase(block.value()) == 1);
        }
        alloc.Free(0, block, iteration + 1);
      }
    });
  }
  for (auto& thread : allocator_threads) thread.join();
  assert(live_offsets.empty());
  dsidle::SwccShardAllocator size_classes(pool, 2);
  const auto small = size_classes.Allocate(1, 97);
  const auto medium = size_classes.Allocate(1, 17'000);
  const auto large = size_classes.Allocate(1, 900'000);
  assert(small && medium && large && small != medium && medium != large);
  size_classes.Free(1, medium, 17'000, 11);
  assert(size_classes.HarvestRemote(1, 17'000) == 1);
  assert(size_classes.Allocate(1, 17'000) == medium);
  dsidle::ConfigureCurrentSwccAllocator(pool, 2, 0);
  const auto bound = dsidle::AllocateCurrentSwcc(129);
  assert(bound && bound.get(dsidle::SharedPoolBase()));
  assert(dsidle::CurrentSwccOwner(bound, 129) == 0);
  dsidle::FreeCurrentSwcc(bound, 129);
  dsidle::ConfigureCurrentSwccAllocator(pool, 2, 1);
  const auto foreign = dsidle::AllocateCurrentSwcc(129);
  assert(dsidle::CurrentSwccOwner(foreign, 129) == 1);
  dsidle::ConfigureCurrentSwccAllocator(pool, 2, 0);
  dsidle::FreeCurrentSwccToOwner(1, foreign, 129, 23);
  assert(size_classes.HarvestRemote(1, 129) == 1);
  assert(size_classes.Allocate(1, 129) == foreign);
  latency_sim::Config latency;
  latency.enabled = true;
  latency.stats_enabled = true;
  latency_sim::GlobalLatencySimulator().Configure(latency);
  {
    latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
    const auto measured = alloc.Allocate(0);
    alloc.Free(0, measured, 31);
    assert(alloc.HarvestRemote(0) == 1);
  }
  const auto latency_stats =
      latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
  assert(latency_stats.hwcc_raw_line_accesses > 0);
  assert(latency_stats.swcc_raw_line_accesses > 0);
  latency_sim::GlobalLatencySimulator().Configure({});
  pool.Close(); unlink(path);
}
