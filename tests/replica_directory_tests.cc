#include "dsidle/replica_directory.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

int main() {
  char path[] = "/tmp/dsidle-replica-test.XXXXXX";
  const int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  auto pool = dsidle::SharedPool::Create(path, {16ULL << 20, 0, 4ULL << 20, 4ULL << 20, 12ULL << 20});
  dsidle::InitializePoolMetadata(pool, {1, 1, 8192});
  dsidle::NodeControlSlab controls(pool);
  const auto ref = controls.Reserve(4ULL << 20, 2);
  controls.Publish(ref, dsidle::MasstreeNodeVersionBits::isleaf_bit);
  dsidle::ReplicaDirectory directory(pool);
  dsidle::ConfigureCurrentReplicaDirectory(directory);

  auto* first = static_cast<char*>(std::malloc(32)); assert(first); std::strcpy(first, "first");
  assert(directory.Publish(ref, {first, 1, 7, 32, dsidle::ReplicaKind::kValueLeaf}) == nullptr);
  directory.RecordAccess(ref);
  directory.RecordAccess(ref);
  assert(directory.AccessCount(ref) == 2);
  {
    auto handle = directory.Acquire(ref, 1, 7);
    assert(handle && std::strcmp(static_cast<char*>(handle.snapshot().local_ptr), "first") == 0);
    assert(!directory.Acquire(ref, 2, 7));
  }
  auto* second = static_cast<char*>(std::malloc(32)); assert(second); std::strcpy(second, "second");
  std::free(directory.Publish(ref, {second, 1, 8, 32, dsidle::ReplicaKind::kValueLeaf}));
  assert(!directory.Acquire(ref, 1, 7));
  auto handle = directory.Acquire(ref, 1, 8);
  assert(handle && std::strcmp(static_cast<char*>(handle.snapshot().local_ptr), "second") == 0);
  handle = {};
  std::free(directory.Invalidate(ref));
  assert(!directory.Acquire(ref, 1, 8));
  controls.Retire(ref, 9);
  controls.Release(ref);
  const auto reused = controls.Reserve((4ULL << 20) + 64, 2);
  assert(reused == ref && directory.AccessCount(reused) == 0);
  pool.Close();
  assert(unlink(path) == 0);
}
