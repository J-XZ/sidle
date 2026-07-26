#include "dsidle/replica_directory.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <unistd.h>

int main() {
  char path[] = "/tmp/dsidle-replica-test.XXXXXX";
  const int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
  auto pool = dsidle::SharedPool::Create(path, {16ULL << 20, 0, 4ULL << 20, 4ULL << 20, 12ULL << 20});
  dsidle::InitializePoolMetadata(pool, {1, 1, 8192});
  dsidle::NodeControlSlab controls(pool);
  const auto ref = controls.Reserve(4ULL << 20, 2);
  controls.Publish(ref, 8);
  dsidle::ReplicaDirectory directory(pool);
  dsidle::ConfigureCurrentReplicaDirectory(directory);
  const dsidle::NodeRef second_segment(
      ref.value() + 4096 * sizeof(dsidle::NodeControl));
  assert(directory.AccessCount(second_segment) == 0);
  directory.RecordAccess(second_segment);
  assert(directory.AccessCount(second_segment) == 1);

  auto* first = static_cast<char*>(std::malloc(32)); assert(first); std::strcpy(first, "first");
  assert(directory.Publish(ref, {first, 1, 8, 32, dsidle::ReplicaKind::kValueLeaf}) == nullptr);
  assert(directory.HasLocalPlacement(ref, 1));
  assert(!directory.HasLocalPlacement(ref, 2));
  directory.RecordAccess(ref);
  directory.RecordAccess(ref);
  assert(directory.AccessCount(ref) == 2);
  directory.HalveAccess(ref);
  assert(directory.AccessCount(ref) == 1);
  {
    auto handle = directory.Acquire(ref, 1, 8);
    assert(handle && std::strcmp(static_cast<char*>(handle.snapshot().local_ptr), "first") == 0);
    assert(!directory.Acquire(ref, 2, 8));
  }
  auto* second = static_cast<char*>(std::malloc(32)); assert(second); std::strcpy(second, "second");
  ref.get(pool.base())->version_and_state.store(16, std::memory_order_release);
  std::free(directory.Publish(ref, {second, 1, 16, 32, dsidle::ReplicaKind::kValueLeaf}));
  assert(!directory.Acquire(ref, 1, 8));
  auto handle = directory.Acquire(ref, 1, 16);
  assert(handle && std::strcmp(static_cast<char*>(handle.snapshot().local_ptr), "second") == 0);
  handle = {};
  ref.get(pool.base())->version_and_state.store(24, std::memory_order_release);
  assert(!directory.Acquire(ref, 1, 24));
  assert(directory.LocalBytes() == 0);
  assert(directory.HasLocalPlacement(ref, 1));
  assert(!directory.Acquire(ref, 1, 16));
  auto* refreshed = static_cast<char*>(std::malloc(32));
  assert(refreshed);
  std::strcpy(refreshed, "refreshed");
  void* refreshed_old = nullptr;
  assert(directory.TryRefresh(
      ref, {refreshed, 1, 24, 32, dsidle::ReplicaKind::kValueLeaf},
      true, &refreshed_old));
  assert(refreshed_old == nullptr);
  assert(directory.Acquire(ref, 1, 24));
  std::free(directory.Invalidate(ref));
  assert(!directory.HasLocalPlacement(ref, 1));
  assert(!directory.Acquire(ref, 1, 16));
  auto* demoted_refresh = static_cast<char*>(std::malloc(32));
  assert(demoted_refresh);
  assert(!directory.TryRefresh(
      ref, {demoted_refresh, 1, 24, 32,
            dsidle::ReplicaKind::kValueLeaf},
      true, &refreshed_old));
  std::free(demoted_refresh);
  directory.SetBudgetBytes(32);
  auto* budgeted = static_cast<char*>(std::malloc(32)); assert(budgeted);
  void* superseded = nullptr;
  assert(directory.TryPublish(ref, {budgeted, 1, 9, 32, dsidle::ReplicaKind::kValueLeaf}, &superseded));
  assert(superseded == nullptr && directory.LocalBytes() == 32);
  auto* oversized = static_cast<char*>(std::malloc(64)); assert(oversized);
  assert(!directory.TryPublish(ref, {oversized, 1, 10, 64, dsidle::ReplicaKind::kValueLeaf}, &superseded));
  std::free(oversized);
  std::free(directory.Invalidate(ref));
  assert(directory.LocalBytes() == 0);
  std::atomic<bool> publishing{true};
  std::thread reader([&] {
    while (publishing.load(std::memory_order_acquire)) {
      auto concurrent = directory.Acquire(ref, 1, 42);
      if (concurrent) assert(static_cast<char*>(concurrent.snapshot().local_ptr)[0] == 'r');
    }
  });
  for (unsigned index = 0; index != 1024; ++index) {
    auto* replacement = static_cast<char*>(std::malloc(32)); assert(replacement);
    replacement[0] = 'r';
    void* old = nullptr;
    assert(directory.TryPublish(ref, {replacement, 1, 42, 32, dsidle::ReplicaKind::kValueLeaf}, &old));
    std::free(old);
  }
  publishing.store(false, std::memory_order_release);
  reader.join();
  std::free(directory.Invalidate(ref));
  auto* retiring = static_cast<char*>(std::malloc(32));
  assert(retiring);
  void* retiring_old = nullptr;
  assert(directory.TryPublish(
      ref, {retiring, 1, 24, 32, dsidle::ReplicaKind::kValueLeaf},
      &retiring_old));
  assert(retiring_old == nullptr);
  assert(directory.HasLocalPlacement(ref, 1));
  controls.Retire(ref, 9);
  assert(!directory.HasLocalPlacement(ref, 1));
  assert(directory.LocalBytes() == 0);
  controls.Release(ref);
  const auto reused = controls.Reserve((4ULL << 20) + 64, 2);
  assert(reused == ref && directory.AccessCount(reused) == 0);
  for (std::uint32_t access = 0; access != 65536; ++access)
    directory.RecordAccess(reused);
  assert(directory.AccessCount(reused) == 0);
  pool.Close();
  assert(unlink(path) == 0);
}
