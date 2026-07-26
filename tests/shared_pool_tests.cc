#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

int main() {
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
