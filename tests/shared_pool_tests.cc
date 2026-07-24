#include "dsidle/shared_pool.h"

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
  const dsidle::PoolLayout layout{8ULL << 20, 0, 2ULL << 20, 2ULL << 20, 6ULL << 20};
  auto pool = dsidle::SharedPool::Create(path, layout);
  assert(pool.header()->magic == dsidle::kPoolMagic);
  assert(pool.root_control()->version.load() == 0);
  const pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    auto attached = dsidle::SharedPool::Attach(path, layout.total_bytes);
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
