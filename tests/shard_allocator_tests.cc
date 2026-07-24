#include "dsidle/shard_allocator.h"
#include <cassert>
#include <cstdio>
#include <unistd.h>
int main() {
  char path[] = "/tmp/dsidle-shard-test.XXXXXX";
  int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  auto pool = dsidle::SharedPool::Create(path, {8ULL<<20, 0, 2ULL<<20, 2ULL<<20, 6ULL<<20});
  dsidle::FixedBlockShardAllocator::Initialize(pool, 2, 64);
  dsidle::FixedBlockShardAllocator alloc(pool, 2, 64);
  const auto first = alloc.Allocate(0); assert(first);
  alloc.Free(0, first, 9); assert(alloc.HarvestRemote(0) == 1);
  const auto reused = alloc.Allocate(0); assert(reused == first);
  pool.Close(); unlink(path);
}
