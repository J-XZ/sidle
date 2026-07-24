#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"

#include "query_masstree.hh"
#include "masstree_struct.hh"
#include "masstree_tcursor.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <vector>

namespace {
struct ScanCollector {
  std::vector<std::pair<std::string, std::string>> rows;
  template <typename S, typename K>
  void visit_leaf(const S&, const K&, threadinfo&) {}
  bool visit_value(lcdf::Str key, row_type* value, threadinfo&) {
    const auto column = value->col(0);
    rows.emplace_back(std::string(key.s, key.len), std::string(column.s, column.len));
    return true;
  }
};

struct ConcurrentScanCollector {
  int signal_fd{-1};
  bool signaled{false};
  template <typename S, typename K>
  void visit_leaf(const S&, const K&, threadinfo&) {
    if (!signaled) {
      const char token = 's';
      assert(write(signal_fd, &token, 1) == 1);
      signaled = true;
      usleep(20'000);
    }
  }
  bool visit_value(lcdf::Str, row_type*, threadinfo&) { return true; }
};
}  // namespace
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
  ti->rcu_start();
  assert(dsidle::SharedEpochSlots(pool).MinimumActive() != dsidle::kEpochInactive);
  ti->rcu_stop();
  assert(dsidle::SharedEpochSlots(pool).MinimumActive() == dsidle::kEpochInactive);

  // A detached canonical leaf exercises the same deallocate_rcu path used by
  // structural removal: its control line must remain RETIRING until the
  // thread's epoch drain returns the paired SWCC body to the free path.
  using test_leaf_type = Masstree::leaf<Masstree::default_table::parameters_type>;
  test_leaf_type* retired_leaf = test_leaf_type::make_root(0, nullptr, *ti);
  const auto retired_ref = retired_leaf->control_ref();
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kPublished);
  retired_leaf->deallocate_rcu(*ti);
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kRetiring);
  ti->rcu_drain();
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kFree);

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

  int start_gate[2];
  assert(pipe(start_gate) == 0);
  const std::string race_values[] = {"race-writer-a", "race-writer-b"};
  pid_t racers[2];
  for (unsigned racer = 0; racer != 2; ++racer) {
    racers[racer] = fork();
    assert(racers[racer] >= 0);
    if (racers[racer] == 0) {
      close(start_gate[1]);
      char token = 0;
      if (read(start_gate[0], &token, 1) != 1) _exit(7);
      pool.Close();
      try {
        auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
        dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
        threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
        Masstree::default_table child_table;
        child_table.table().attach();
        query.run_replace(child_table.table(), lcdf::Str(writer_key.data(), writer_key.size()),
                          lcdf::Str(race_values[racer].data(), race_values[racer].size()), *child_ti);
      } catch (...) {
        _exit(8);
      }
      _exit(0);
    }
  }
  close(start_gate[0]);
  assert(write(start_gate[1], "++", 2) == 2);
  close(start_gate[1]);
  for (const pid_t racer : racers) {
    int status = 0;
    assert(waitpid(racer, &status, 0) == racer);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  lcdf::Str raced;
  assert(query.run_get1(table.table(), lcdf::Str(writer_key.data(), writer_key.size()), 0, raced, *ti));
  const std::string raced_value(raced.s, raced.len);
  assert(raced_value == race_values[0] || raced_value == race_values[1]);
  expected[writer_key] = raced_value;

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

  std::vector<std::pair<std::string, std::string>> split_inserts;
  for (unsigned i = 0; i != 48; ++i) {
    std::string key(8, 'z');
    key[0] = static_cast<char>(i + 1);
    split_inserts.emplace_back(std::move(key), "split-value-" + std::to_string(i));
  }
  const pid_t splitter = fork();
  assert(splitter >= 0);
  if (splitter == 0) {
    pool.Close();
    try {
      auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
      dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
      threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
      Masstree::default_table child_table;
      child_table.table().attach();
      for (const auto& [key, value] : split_inserts)
        query.run_replace(child_table.table(), lcdf::Str(key.data(), key.size()),
                          lcdf::Str(value.data(), value.size()), *child_ti);
    } catch (...) {
      _exit(9);
    }
    _exit(0);
  }
  int splitter_status = 0;
  assert(waitpid(splitter, &splitter_status, 0) == splitter);
  assert(WIFEXITED(splitter_status) && WEXITSTATUS(splitter_status) == 0);
  for (const auto& [key, value] : split_inserts) {
    expected[key] = value;
    lcdf::Str inserted;
    assert(query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, inserted, *ti));
    assert(inserted.len == value.size() && std::memcmp(inserted.s, value.data(), inserted.len) == 0);
  }

  // Two independently attached workers begin their disjoint insert batches at
  // the same time. Both batches exceed a leaf's capacity and therefore force
  // overlapping split/root publication paths rather than merely racing a
  // value overwrite.
  std::vector<std::pair<std::string, std::string>> concurrent_inserts;
  for (unsigned worker = 0; worker != 2; ++worker) {
    for (unsigned i = 0; i != 64; ++i) {
      std::string key(8, static_cast<char>('q' + worker));
      key[0] = static_cast<char>(0x80 + worker);
      key[1] = static_cast<char>(i + 1);
      concurrent_inserts.emplace_back(std::move(key),
                                      "concurrent-split-" + std::to_string(worker) + "-" + std::to_string(i));
    }
  }
  int split_ready[2];
  int split_go[2];
  assert(pipe(split_ready) == 0 && pipe(split_go) == 0);
  pid_t split_workers[2]{};
  for (unsigned worker = 0; worker != 2; ++worker) {
    split_workers[worker] = fork();
    assert(split_workers[worker] >= 0);
    if (split_workers[worker] == 0) {
      close(split_ready[0]);
      close(split_go[1]);
      pool.Close();
      try {
        auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
        dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
        threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, static_cast<int>(worker));
        Masstree::default_table child_table;
        child_table.table().attach();
        const char ready = 'r';
        if (write(split_ready[1], &ready, 1) != 1) _exit(14);
        char go = 0;
        if (read(split_go[0], &go, 1) != 1) _exit(15);
        for (unsigned i = 0; i != 64; ++i) {
          const auto& [key, value] = concurrent_inserts[worker * 64 + i];
          query.run_replace(child_table.table(), lcdf::Str(key.data(), key.size()),
                            lcdf::Str(value.data(), value.size()), *child_ti);
        }
        child_ti->rcu_drain();
      } catch (...) {
        _exit(16);
      }
      _exit(0);
    }
  }
  close(split_ready[1]);
  close(split_go[0]);
  for (unsigned worker = 0; worker != 2; ++worker) {
    char ready = 0;
    assert(read(split_ready[0], &ready, 1) == 1 && ready == 'r');
  }
  const char go = 'g';
  assert(write(split_go[1], &go, 1) == 1);
  assert(write(split_go[1], &go, 1) == 1);
  close(split_ready[0]);
  close(split_go[1]);
  for (const auto split_worker : split_workers) {
    int status = 0;
    assert(waitpid(split_worker, &status, 0) == split_worker);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  for (const auto& [key, value] : concurrent_inserts) {
    expected[key] = value;
    lcdf::Str inserted;
    assert(query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, inserted, *ti));
    assert(inserted.len == value.size() && std::memcmp(inserted.s, value.data(), inserted.len) == 0);
  }

  int scan_started[2];
  assert(pipe(scan_started) == 0);
  const std::string scan_delete_key = split_inserts.back().first;
  const pid_t scan_remover = fork();
  assert(scan_remover >= 0);
  if (scan_remover == 0) {
    close(scan_started[1]);
    char token = 0;
    if (read(scan_started[0], &token, 1) != 1) _exit(10);
    pool.Close();
    try {
      auto attached = dsidle::SharedPool::Attach(path, kPoolBytes);
      dsidle::ConfigureCurrentSwccAllocator(attached, 1, 0);
      // The parent sent the token from visit_leaf(), while the public scan
      // wrapper's RCU scope is still live across the complete leaf walk.
      if (dsidle::SharedEpochSlots(attached).MinimumActive() == dsidle::kEpochInactive)
        _exit(13);
      threadinfo* child_ti = threadinfo::make(threadinfo::TI_MAIN, 0);
      Masstree::default_table child_table;
      child_table.table().attach();
      if (!query.run_remove(child_table.table(), lcdf::Str(scan_delete_key.data(), scan_delete_key.size()), *child_ti))
        _exit(11);
    } catch (...) {
      _exit(12);
    }
    _exit(0);
  }
  close(scan_started[0]);
  ConcurrentScanCollector concurrent_scanner{scan_started[1]};
  assert(table.table().scan(lcdf::Str("", 0), true, concurrent_scanner, *ti) >= 0);
  close(scan_started[1]);
  int scan_remover_status = 0;
  assert(waitpid(scan_remover, &scan_remover_status, 0) == scan_remover);
  assert(WIFEXITED(scan_remover_status) && WEXITSTATUS(scan_remover_status) == 0);
  expected.erase(scan_delete_key);
  lcdf::Str scan_deleted;
  assert(!query.run_get1(table.table(), lcdf::Str(scan_delete_key.data(), scan_delete_key.size()), 0, scan_deleted, *ti));

  ScanCollector scanner;
  assert(table.table().scan(lcdf::Str("", 0), true, scanner, *ti) ==
         static_cast<int>(expected.size()));
  assert(scanner.rows.size() == expected.size());
  auto expected_it = expected.begin();
  for (const auto& [key, value] : scanner.rows) {
    assert(expected_it != expected.end());
    assert(key == expected_it->first && value == expected_it->second);
    ++expected_it;
  }
  assert(expected_it == expected.end());

  // The original thread accumulated retired overwrite/delete values. Its
  // explicit exit path must reclaim them and leave its shared slot inactive.
  ti->rcu_drain();
  assert(dsidle::SharedEpochSlots(pool).MinimumActive() == dsidle::kEpochInactive);

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

  remounted_ti->rcu_drain();
  assert(dsidle::SharedEpochSlots(remapped).MinimumActive() == dsidle::kEpochInactive);

  remapped.Close();
  assert(munmap(guard, kPoolBytes) == 0);
  assert(unlink(path) == 0);
}
