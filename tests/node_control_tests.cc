#include "dsidle/shared_pool.h"

#include <cassert>
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
  auto* canonical = reinterpret_cast<std::uint64_t*>(
      static_cast<std::byte*>(pool.base()) + (2ULL << 20));
  *canonical = 0x5349444c45ULL;
  dsidle::FlushSwccRange(canonical, sizeof(*canonical));
  assert(dsidle::ResolveCanonicalNode<std::uint64_t>(ref) == canonical);
  assert(*dsidle::ResolveCanonicalNode<std::uint64_t>(ref) == 0x5349444c45ULL);
  dsidle::RootControlAccessor root(pool.root_control());
  root.publish(ref, 1);
  const auto root_view = root.stable();
  assert(root_view.ref == ref && root_view.generation == 1 && root_view.version == 1);
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
  slab.Release(ref);

  pool.Close();
  assert(unlink(path) == 0);
}
