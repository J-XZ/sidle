#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"

#include "query_masstree.hh"
#include "masstree_struct.hh"
#include "masstree_tcursor.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  char path[] = "/tmp/dsidle-masstree-test.XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);

  constexpr std::uint64_t kPoolBytes = 128ULL << 20;
  auto pool = dsidle::SharedPool::Create(path, {kPoolBytes, 0, 32ULL << 20, 32ULL << 20, 96ULL << 20});
  dsidle::InitializePoolMetadata(pool, {1, 2, 1024});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 1);
  dsidle::ConfigureCurrentSwccAllocator(pool, 1, 0);

  threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, 0);
  Masstree::default_table table;
  table.initialize(*ti, 80);
  const auto root_ref = table.table().root()->control_ref();
  assert(root_ref);
  const dsidle::NodeVersionAccessor root_version(pool.base(), root_ref);
  const auto root_view = root_version.stable();
  assert(root_view.v & dsidle::MasstreeNodeVersionBits::isleaf_bit);
  assert(root_view.swcc_off == static_cast<std::uint64_t>(
      reinterpret_cast<std::byte*>(table.table().root()) - static_cast<std::byte*>(pool.base())));
  const auto published_root = dsidle::RootControlAccessor(pool.root_control()).stable();
  assert(published_root.ref == root_ref && published_root.generation == root_view.gen);
  query<row_type> query;
  std::map<std::string, std::string> expected;
  const auto fixed_key = [](std::uint64_t number) {
    std::string key(8, 'k');
    key[0] = static_cast<char>('A' + number);
    return key;
  };
  constexpr unsigned kEntryCount = 64;
  for (unsigned i = 0; i != kEntryCount; ++i) {
    const std::string key = fixed_key((i * 13) % kEntryCount);
    const std::string value = "value-" + std::to_string(i);
    expected[key] = value;
    query.run_replace(table.table(), lcdf::Str(key.data(), key.size()),
                      lcdf::Str(value.data(), value.size()), *ti);
  }
  for (unsigned i = 0; i != 2; ++i) {
    const std::string key = "prefix--" + std::string(160, 'x') + static_cast<char>('A' + i);
    const std::string value = "long-value-" + std::to_string(i);
    expected[key] = value;
    query.run_replace(table.table(), lcdf::Str(key.data(), key.size()),
                      lcdf::Str(value.data(), value.size()), *ti);
  }
  for (unsigned i = 0; i < kEntryCount; i += 7) {
    const std::string key = fixed_key(i);
    const std::string value = "updated-" + std::to_string(i);
    expected[key] = value;
    query.run_replace(table.table(), lcdf::Str(key.data(), key.size()),
                      lcdf::Str(value.data(), value.size()), *ti);
  }

  for (const auto& [key, expected_value] : expected) {
    lcdf::Str value;
    assert(query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, value, *ti));
    assert(value.len == expected_value.size());
    assert(std::memcmp(value.s, expected_value.data(), value.len) == 0);
  }

  const pid_t reader = fork();
  assert(reader >= 0);
  if (reader == 0) {
    pool.Close();
    try {
      auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
      dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
      threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
      Masstree::default_table child_table;
      child_table.table().attach();
      for (const auto& [key, expected_value] : expected) {
        lcdf::Str value;
        if (!query.run_get1(child_table.table(), lcdf::Str(key.data(), key.size()), 0, value, *child_ti) ||
            value.len != expected_value.size() ||
            std::memcmp(value.s, expected_value.data(), value.len) != 0)
          _exit(2);
      }
    } catch (...) {
      _exit(3);
    }
    _exit(0);
  }
  int reader_status = 0;
  assert(waitpid(reader, &reader_status, 0) == reader);
  assert(WIFEXITED(reader_status) && WEXITSTATUS(reader_status) == 0);

  const std::string writer_key = expected.begin()->first;
  const std::string writer_value = "child-overwrite";
  const pid_t writer = fork();
  assert(writer >= 0);
  if (writer == 0) {
    pool.Close();
    try {
      auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
      dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
      threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
      Masstree::default_table child_table;
      child_table.table().attach();
      query.run_replace(child_table.table(), lcdf::Str(writer_key.data(), writer_key.size()),
                        lcdf::Str(writer_value.data(), writer_value.size()), *child_ti);
    } catch (...) {
      _exit(4);
    }
    _exit(0);
  }
  int writer_status = 0;
  assert(waitpid(writer, &writer_status, 0) == writer);
  assert(WIFEXITED(writer_status) && WEXITSTATUS(writer_status) == 0);
  expected[writer_key] = writer_value;
  lcdf::Str overwritten;
  assert(query.run_get1(table.table(), lcdf::Str(writer_key.data(), writer_key.size()), 0, overwritten, *ti));
  assert(overwritten.len == writer_value.size());
  assert(std::memcmp(overwritten.s, writer_value.data(), overwritten.len) == 0);

  auto delete_it = expected.begin();
  if (delete_it->first == writer_key) ++delete_it;
  const std::string delete_key = delete_it->first;
  const pid_t remover = fork();
  assert(remover >= 0);
  if (remover == 0) {
    pool.Close();
    try {
      auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
      dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
      threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
      Masstree::default_table child_table;
      child_table.table().attach();
      if (!query.run_remove(child_table.table(), lcdf::Str(delete_key.data(), delete_key.size()), *child_ti))
        _exit(5);
    } catch (...) {
      _exit(6);
    }
    _exit(0);
  }
  int remover_status = 0;
  assert(waitpid(remover, &remover_status, 0) == remover);
  assert(WIFEXITED(remover_status) && WEXITSTATUS(remover_status) == 0);
  expected.erase(delete_key);
  lcdf::Str deleted;
  assert(!query.run_get1(table.table(), lcdf::Str(delete_key.data(), delete_key.size()), 0, deleted, *ti));

  const auto original_base = pool.base();
  pool.Close();
  void* guard = mmap(original_base, kPoolBytes, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  assert(guard == original_base);
  void* target = mmap(nullptr, kPoolBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(target != MAP_FAILED && target != original_base);
  assert(munmap(target, kPoolBytes) == 0);

  auto remapped = dsidle::SharedPool::AttachAt(path, kPoolBytes, target);
  assert(remapped.base() != original_base);
  dsidle::ConfigureCurrentSwccAllocator(remapped, 1, 0);
  threadinfo* remounted_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
  Masstree::default_table remounted;
  remounted.table().attach();
  for (const auto& [key, expected_value] : expected) {
    lcdf::Str value;
    assert(query.run_get1(remounted.table(), lcdf::Str(key.data(), key.size()), 0, value, *remounted_ti));
    assert(value.len == expected_value.size());
    assert(std::memcmp(value.s, expected_value.data(), value.len) == 0);
  }

  remapped.Close();
  assert(munmap(guard, kPoolBytes) == 0);
  assert(unlink(path) == 0);
}
