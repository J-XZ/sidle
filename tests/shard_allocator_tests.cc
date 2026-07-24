#include "dsidle/shard_allocator.h"
#include <cassert>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
int main() {
  char path[] = "/tmp/dsidle-shard-test.XXXXXX";
  int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  auto pool = dsidle::SharedPool::Create(path, {8ULL<<20, 0, 2ULL<<20, 2ULL<<20, 6ULL<<20});
  dsidle::InitializePoolMetadata(pool, {2, 2, 64});
  dsidle::NodeControlSlab controls(pool);
  const auto node = controls.Acquire(2ULL << 20, 7);
  assert(node && node.get(pool.base())->generation == 1);
  node.get(pool.base())->allocation_state = dsidle::NodeAllocationState::kRetiring;
  controls.Release(node);
  const auto reused_node = controls.Acquire((2ULL << 20) + 64, 8);
  assert(reused_node == node && reused_node.get(pool.base())->generation == 2);
  dsidle::FixedBlockShardAllocator::Initialize(pool, 2, 64);
  dsidle::FixedBlockShardAllocator alloc(pool, 2, 64);
  const auto first = alloc.Allocate(0); assert(first);
  const auto pid = fork(); assert(pid >= 0);
  if (pid == 0) {
    auto attached = dsidle::SharedPool::Attach(path, 8ULL << 20);
    dsidle::FixedBlockShardAllocator child_alloc(attached, 2, 64);
    child_alloc.Free(0, first, 9);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(alloc.HarvestRemote(0) == 1);
  const auto reused = alloc.Allocate(0); assert(reused == first);
  pool.Close(); unlink(path);
}
