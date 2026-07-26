#include "dsidle/shared_pool.h"

#include <atomic>
#include <cassert>
#include <thread>
#include <unistd.h>

int main() {
  char path[] = "/tmp/dsidle-node-control-test.XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);

  auto pool = dsidle::SharedPool::Create(path, {8ULL << 20, 0, 2ULL << 20, 2ULL << 20, 6ULL << 20});
  dsidle::InitializePoolMetadata(pool, {1, 1, 4});
  dsidle::NodeControlSlab slab(pool);
  const auto ref = slab.Reserve(2ULL << 20, 1);
  assert(ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kAllocating);
  assert(ref.get(pool.base())->generation == 1);
  slab.Publish(ref, dsidle::MasstreeNodeVersionBits::isleaf_bit);
  assert(dsidle::TryLockLeafLink(ref));
  assert(!dsidle::TryLockLeafLink(ref));
  dsidle::UnlockLeafLink(ref);
  assert(dsidle::TryLockLeafLink(ref));
  dsidle::UnlockLeafLink(ref);
  auto* canonical = reinterpret_cast<std::uint64_t*>(
      static_cast<std::byte*>(pool.base()) + (2ULL << 20));
  *canonical = 0x5349444c45ULL;
  dsidle::FlushSwccRange(canonical, sizeof(*canonical));
  assert(dsidle::ResolveCanonicalNode<std::uint64_t>(ref) == canonical);
  assert(*dsidle::ResolveCanonicalNode<std::uint64_t>(ref) == 0x5349444c45ULL);
  dsidle::RootControlAccessor root(pool.root_control());
  root.publish(ref, 1);
  const auto root_view = root.stable();
  assert(root_view.ref == ref && root_view.generation == 1 && root_view.version == 2);
  const auto ref2 = slab.Reserve((2ULL << 20) + 64, 1);
  slab.Publish(ref2, dsidle::MasstreeNodeVersionBits::isleaf_bit);
  std::atomic<bool> writers_done{false};
  std::thread writer_a([&] {
    for (int i = 0; i != 10'000; ++i)
      root.publish(ref, 11);
  });
  std::thread writer_b([&] {
    for (int i = 0; i != 10'000; ++i)
      root.publish(ref2, 22);
    writers_done.store(true, std::memory_order_release);
  });
  while (!writers_done.load(std::memory_order_acquire)) {
    const auto view = root.stable();
    assert((view.ref == ref && (view.generation == 1 || view.generation == 11)) ||
           (view.ref == ref2 && view.generation == 22));
    assert((view.version & 1U) == 0);
  }
  writer_a.join();
  writer_b.join();
  dsidle::NodeVersionAccessor version(pool.base(), ref);
  const auto initial = version.stable();
  assert(initial.swcc_off == (2ULL << 20) && initial.gen == 1 && !initial.locked());
  assert(initial.v == dsidle::MasstreeNodeVersionBits::isleaf_bit);

  std::uint64_t locked = 0;
  assert(version.try_lock(&locked));
  version.mark_insert();
  version.unlock_release(locked | dsidle::MasstreeNodeVersionBits::inserting_bit);
  const auto inserted = version.stable();
  assert(inserted.v == (dsidle::MasstreeNodeVersionBits::isleaf_bit |
                        dsidle::MasstreeNodeVersionBits::vinsert_lowbit));

  assert(version.try_lock(&locked));
  version.mark_split();
  version.unlock_release(locked | dsidle::MasstreeNodeVersionBits::splitting_bit);
  const auto split = version.stable();
  assert(split.v == (dsidle::MasstreeNodeVersionBits::isleaf_bit |
                     dsidle::MasstreeNodeVersionBits::vsplit_lowbit));

  slab.Retire(ref, 7);
  assert(ref.get(pool.base())->retire_epoch == 7);
  assert(dsidle::ResolveCanonicalNode<std::uint64_t>(ref) == canonical);
  assert(version.stable().v == split.v);
  bool retiring_lock_rejected = false;
  try {
    (void) version.try_lock();
  } catch (const std::runtime_error&) {
    retiring_lock_rejected = true;
  }
  assert(retiring_lock_rejected);
  slab.Release(ref);
  slab.Retire(ref2, 7);
  slab.Release(ref2);

  pool.Close();
  assert(unlink(path) == 0);
}
