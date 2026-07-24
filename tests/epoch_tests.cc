#include "dsidle/epoch.h"
#include "dsidle/shared_pool.h"
#include <cassert>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  dsidle::EpochTable table(2,2);
  assert(table.MinimumActive()==dsidle::kEpochInactive);
  table.Enter(0,0,8); table.Enter(1,1,3); assert(table.MinimumActive()==3);
  table.Leave(1,1); assert(table.MinimumActive()==8);
  table.Leave(0,0); assert(table.MinimumActive()==dsidle::kEpochInactive);

  char path[] = "/tmp/dsidle-epoch-test.XXXXXX";
  const int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  constexpr std::uint64_t kPoolBytes = 16ULL << 20;
  auto pool = dsidle::SharedPool::Create(path, {kPoolBytes, 0, 4ULL << 20, 4ULL << 20, 12ULL << 20});
  dsidle::InitializePoolMetadata(pool, {2, 2, 16});
  auto epochs = dsidle::SharedEpochSlots(pool);
  const auto clock = dsidle::SharedEpochState(pool);
  assert(clock.Current() == 1);
  assert(clock.Advance() == 2);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    pool.Close();
    auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
    assert(dsidle::SharedEpochState(attached).Advance() == 3);
    dsidle::SharedEpochSlots(attached).Enter(1, 1, 17);
    _exit(0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(clock.Current() == 3);
  assert(epochs.MinimumActive() == 17);
  epochs.Leave(1, 1);
  assert(epochs.MinimumActive() == dsidle::kEpochInactive);
  pool.Close();
  assert(unlink(path) == 0);
}
