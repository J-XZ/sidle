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
  const auto ref = slab.Acquire(2ULL << 20, 1);
  dsidle::RootControlAccessor root(pool.root_control());
  root.publish(ref, 1);
  const auto root_view = root.stable();
  assert(root_view.ref == ref && root_view.generation == 1 && root_view.version == 1);
  dsidle::NodeVersionAccessor version(pool.base(), ref);
  const auto initial = version.stable();
  assert(initial.swcc_off == (2ULL << 20) && initial.gen == 1 && !initial.locked());

  std::uint64_t locked = 0;
  assert(version.try_lock(&locked));
  version.mark_insert();
  version.unlock_release(locked | dsidle::MasstreeNodeVersionBits::inserting_bit);
  const auto inserted = version.stable();
  assert(inserted.v == dsidle::MasstreeNodeVersionBits::vinsert_lowbit);

  assert(version.try_lock(&locked));
  version.mark_split();
  version.unlock_release(locked | dsidle::MasstreeNodeVersionBits::splitting_bit);
  const auto split = version.stable();
  assert(split.v == dsidle::MasstreeNodeVersionBits::vsplit_lowbit);

  pool.Close();
  assert(unlink(path) == 0);
}
