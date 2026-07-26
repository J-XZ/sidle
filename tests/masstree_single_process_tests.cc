#include "dsidle/shared_pool.h"
#include "dsidle/shard_allocator.h"
#include "dsidle/replica_directory.h"
#include "dsidle/latency_simulator.h"

#include "query_masstree.hh"
#include "masstree_struct.hh"
#include "masstree_tcursor.hh"
#include "masstree_get.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "masstree_replica.hh"
#include "masstree_internal_replica.hh"
#include "masstree_root_replica.hh"
#include "masstree_replica_worker.hh"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <vector>

namespace {
std::uint64_t CacheLinesTouched(const void* address, std::size_t bytes,
                                std::size_t line_bytes = 64) {
  if (!address || !bytes) return 0;
  const auto first = reinterpret_cast<std::uintptr_t>(address) / line_bytes;
  const auto last =
      (reinterpret_cast<std::uintptr_t>(address) + bytes - 1) / line_bytes;
  return last - first + 1;
}

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

struct ReplicaPointerScanCollector {
  const std::byte* pool_begin;
  const std::byte* pool_end;
  bool used_replica{false};
  template <typename S, typename K>
  void visit_leaf(const S&, const K&, threadinfo&) {}
  bool visit_value(lcdf::Str, row_type* value, threadinfo&) {
    const auto* address = reinterpret_cast<const std::byte*>(value);
    used_replica = address < pool_begin || address >= pool_end;
    return false;
  }
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
  dsidle::InitializePoolMetadata(pool, {1, 5, 1024});
  dsidle::FixedBlockShardAllocator::InitializeAll(pool, 1);
  dsidle::FinalizePoolInitialization(pool);
  dsidle::ConfigureCurrentSwccAllocator(pool, 1, 0);
  dsidle::ReplicaDirectory replicas(pool);
  dsidle::ConfigureCurrentReplicaDirectory(replicas);

  threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, 0);
  ti->rcu_start();
  assert(dsidle::SharedEpochSlots(pool).MinimumActive() != dsidle::kEpochInactive);
  ti->rcu_stop();
  assert(dsidle::SharedEpochSlots(pool).MinimumActive() == dsidle::kEpochInactive);

  // Exhaust NodeControls after the SWCC body allocation point. The canonical
  // allocation guard must return that body when Reserve throws, so the next
  // successful leaf allocation reuses it without advancing the class bump.
  using test_leaf_type = Masstree::leaf<Masstree::default_table::parameters_type>;
  dsidle::NodeControlSlab rollback_controls(pool);
  std::vector<dsidle::NodeRef> held_controls;
  while (true) {
    try {
      held_controls.push_back(rollback_controls.Reserve(
          pool.header()->swcc_offset + pool.header()->swcc_bytes - 64, 2));
    } catch (const std::runtime_error&) {
      break;
    }
  }
  assert(held_controls.size() == pool.static_layout()->node_control_capacity);
  const auto constructed_leaf_size =
      (sizeof(test_leaf_type) + 63) & ~std::size_t(63);
  const auto leaf_size =
      (constructed_leaf_size + memdebug_size + 63) & ~std::size_t(63);
  const auto leaf_block =
      dsidle::SwccShardAllocator::SizeClassBlockSize(leaf_size);
  std::uint32_t leaf_class = 0;
  for (auto size = dsidle::kSmallestSwccBlock; size < leaf_block; size <<= 1)
    ++leaf_class;
  auto* leaf_control = reinterpret_cast<dsidle::ShardControl*>(
      static_cast<std::byte*>(pool.base()) +
      pool.static_layout()->shard_controls_offset +
      leaf_class * sizeof(dsidle::ShardControl));
  const auto bump_before_failure =
      leaf_control->bump.load(std::memory_order_acquire);
  bool reserve_failure_observed = false;
  try {
    (void) test_leaf_type::make_root(0, nullptr, *ti);
  } catch (const std::runtime_error&) {
    reserve_failure_observed = true;
  }
  assert(reserve_failure_observed);
  const auto bump_after_failure =
      leaf_control->bump.load(std::memory_order_acquire);
  assert(bump_after_failure == bump_before_failure + leaf_block);
  rollback_controls.Cancel(held_controls.back());
  held_controls.pop_back();
  test_leaf_type* recovered_leaf =
      test_leaf_type::make_root(0, nullptr, *ti);
  assert(leaf_control->bump.load(std::memory_order_acquire) ==
         bump_after_failure);
  recovered_leaf->deallocate_rcu(*ti);
  ti->rcu_drain();
  for (const auto ref : held_controls)
    rollback_controls.Cancel(ref);

  // A detached canonical leaf exercises the same deallocate_rcu path used by
  // structural removal: its control line must remain RETIRING until the
  // thread's epoch drain returns the paired SWCC body to the free path.
  test_leaf_type* retired_leaf = test_leaf_type::make_root(0, nullptr, *ti);
  const auto retired_ref = retired_leaf->control_ref();
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kPublished);
  retired_leaf->deallocate_rcu(*ti);
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kRetiring);
  ti->rcu_drain();
  assert(retired_ref.get(pool.base())->allocation_state == dsidle::NodeAllocationState::kFree);

  Masstree::default_table table;
  table.initialize(*ti, 0);
  const auto root_ref = table.table().root()->control_ref();
  assert(root_ref);
  const dsidle::NodeVersionAccessor root_version(pool.base(), root_ref);
  const auto root_view = root_version.stable();
  assert(root_view.v & dsidle::MasstreeNodeVersionBits::isleaf_bit);
  assert(dsidle::LoadCanonicalNodeBytes(root_ref) ==
         constructed_leaf_size);
  assert(root_view.swcc_off == static_cast<std::uint64_t>(
      reinterpret_cast<std::byte*>(table.table().root()) - static_cast<std::byte*>(pool.base())));
  const auto published_root = dsidle::RootControlAccessor(pool.root_control()).stable();
  assert(published_root.ref == root_ref && published_root.generation == root_view.gen);
  query<row_type> query;
  std::map<std::string, std::string> expected;
  using table_params = Masstree::default_table::parameters_type;
  using table_key = Masstree::key<typename table_params::ikey_type>;
  const auto find_slot = [](Masstree::leaf<table_params>* leaf,
                            const std::string& text) {
    table_key key(text.data(), text.size());
    const auto permutation = leaf->permutation();
    for (int index = 0; index != permutation.size(); ++index) {
      const int slot = permutation[index];
      if (leaf->ikey(slot) == key.ikey() && leaf->ksuf_equals(slot, key))
        return slot;
    }
    return -1;
  };
  const std::string cow_key_a(32, 'a');
  const std::string cow_key_b(32, 'b');
  query.run_replace(table.table(), lcdf::Str(cow_key_a.data(), cow_key_a.size()),
                    lcdf::Str("cow-a", 5), *ti);
  table_key cow_search(cow_key_a.data(), cow_key_a.size());
  typename Masstree::node_base<table_params>::nodeversion_type cow_version;
  auto* cow_leaf = table.table().root()->reach_leaf(cow_search, cow_version, *ti);
  const int cow_slot = find_slot(cow_leaf, cow_key_a);
  assert(cow_slot >= 0 && cow_leaf->ksuf_external());
  const lcdf::Str old_suffix = cow_leaf->ksuf_storage(cow_slot);
  const std::string old_suffix_copy(old_suffix.s, old_suffix.len);
  query.run_replace(table.table(), lcdf::Str(cow_key_b.data(), cow_key_b.size()),
                    lcdf::Str("cow-b", 5), *ti);
  cow_leaf = table.table().root()->reach_leaf(cow_search, cow_version, *ti);
  const int new_cow_slot = find_slot(cow_leaf, cow_key_a);
  assert(new_cow_slot >= 0);
  const lcdf::Str new_suffix = cow_leaf->ksuf_storage(new_cow_slot);
  assert(new_suffix.s != old_suffix.s);
  assert(std::string(old_suffix.s, old_suffix.len) == old_suffix_copy);
  if (latency_sim::TscSpinAvailableForTest()) {
    const auto cow_permutation = cow_leaf->permutation();
    using cow_leaf_type = Masstree::leaf<table_params>;
    using cow_replica_type = Masstree::leaf_replica<table_params>;
    auto* external =
        static_cast<typename cow_leaf_type::external_ksuf_type*>(cow_leaf->ksuf_);
    assert(external);
    latency_sim::Config suffix_latency;
    suffix_latency.enabled = true;
    suffix_latency.stats_enabled = true;
    suffix_latency.cache_line_bytes = 1;
    latency_sim::GlobalLatencySimulator().Configure(suffix_latency);
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      const auto measured_suffix = cow_leaf->ksuf_storage(new_cow_slot);
      assert(measured_suffix.len == new_suffix.len);
    }
    const auto suffix_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(suffix_stats.swcc_raw_line_accesses ==
           cow_leaf_type::external_ksuf_type::overhead(cow_leaf_type::width) +
               static_cast<std::uint64_t>(new_suffix.len));

    std::uint64_t expected_swcc_lines =
        CacheLinesTouched(
            external,
            cow_leaf_type::external_ksuf_type::overhead(
                cow_leaf_type::width));
    for (int index = 0; index < cow_permutation.size(); ++index) {
      const int slot = cow_permutation[index];
      if (cow_leaf->has_ksuf(slot)) {
        const auto suffix = cow_leaf->ksuf_storage(slot);
        expected_swcc_lines += CacheLinesTouched(suffix.s, suffix.len);
      }
      if (!cow_leaf->is_layer(slot)) {
        const auto value = cow_leaf->lv_[slot].value();
        assert(value);
        expected_swcc_lines += CacheLinesTouched(value, value->size());
      }
    }
    latency_sim::Config latency;
    latency.enabled = true;
    latency.stats_enabled = true;
    latency_sim::GlobalLatencySimulator().Configure(latency);
    void* measured_replica = nullptr;
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kMerge);
      measured_replica =
          cow_replica_type::Create(*cow_leaf, cow_permutation);
    }
    const auto stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(stats.swcc_raw_line_accesses == expected_swcc_lines);
    std::free(measured_replica);
    latency_sim::GlobalLatencySimulator().Configure({});
  }
  expected[cow_key_a] = "cow-a";
  expected[cow_key_b] = "cow-b";
  lcdf::Str held_value;
  assert(query.run_get1(table.table(), lcdf::Str(cow_key_a.data(), cow_key_a.size()),
                        0, held_value, *ti));
  assert(reinterpret_cast<std::uintptr_t>(held_value.s) <
             reinterpret_cast<std::uintptr_t>(pool.base()) ||
         reinterpret_cast<std::uintptr_t>(held_value.s) >=
             reinterpret_cast<std::uintptr_t>(pool.base()) + kPoolBytes);
  for (unsigned update = 0; update != 128; ++update) {
    const std::string value = "cow-update-" + std::to_string(update);
    query.run_replace(table.table(), lcdf::Str(cow_key_a.data(), cow_key_a.size()),
                      lcdf::Str(value.data(), value.size()), *ti);
    expected[cow_key_a] = value;
  }
  ti->rcu_drain();
  assert(std::string(held_value.s, held_value.len) == "cow-a");
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

  lcdf::Json limited_scan =
      lcdf::Json::array(0, 0, lcdf::Str("", 0), 3);
  query.run_scan(table.table(), limited_scan, *ti);
  assert(limited_scan.size() == 2 + 2 * 3);
  lcdf::Json unlimited_scan =
      lcdf::Json::array(0, 0, lcdf::Str("", 0), 0);
  query.run_scan(table.table(), unlimited_scan, *ti);
  assert(unlimited_scan.size() ==
         2 + 2 * static_cast<int>(expected.size()));

  for (const auto& [key, expected_value] : expected) {
    lcdf::Str value;
    assert(query.run_get1(table.table(), lcdf::Str(key.data(), key.size()), 0, value, *ti));
    assert(value.len == expected_value.size());
    assert(std::memcmp(value.s, expected_value.data(), value.len) == 0);
  }
  using replica_params = Masstree::default_table::parameters_type;
  using replica_key_type = Masstree::key<typename replica_params::ikey_type>;
  using replica_node_type = Masstree::node_base<replica_params>;
  // A layer-leaf replica must advance the key by one ikey before descending.
  // This catches the cross-VM e2e09 path, whose 32-byte keys share prefixes.
  const std::string layered_key =
      "prefix--" + std::string(160, 'x') + 'A';
  replica_key_type layered_search(layered_key.data(), layered_key.size());
  typename replica_node_type::nodeversion_type layered_version;
  auto* layered_leaf =
      table.table().root()->reach_leaf(layered_search, layered_version, *ti);
  if (latency_sim::TscSpinAvailableForTest()) {
    const auto permutation = layered_leaf->permutation();
    std::uint64_t expected_swcc_lines = 0;
    if (layered_leaf->ksuf_external()) {
      using layered_leaf_type = Masstree::leaf<replica_params>;
      const auto* external =
          static_cast<typename layered_leaf_type::external_ksuf_type*>(
              layered_leaf->ksuf_);
      expected_swcc_lines += CacheLinesTouched(
          external,
          layered_leaf_type::external_ksuf_type::overhead(
              layered_leaf_type::width));
    }
    for (int index = 0; index < permutation.size(); ++index) {
      const int slot = permutation[index];
      if (layered_leaf->has_ksuf(slot)) {
        const auto suffix = layered_leaf->ksuf_storage(slot);
        expected_swcc_lines += CacheLinesTouched(suffix.s, suffix.len);
      }
      if (!layered_leaf->is_layer(slot)) {
        const auto value = layered_leaf->lv_[slot].value();
        expected_swcc_lines += CacheLinesTouched(value, value->size());
      }
    }
    latency_sim::Config latency;
    latency.enabled = true;
    latency.stats_enabled = true;
    latency_sim::GlobalLatencySimulator().Configure(latency);
    void* layered_snapshot = nullptr;
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kMerge);
      layered_snapshot =
          Masstree::leaf_replica<replica_params>::Create(
              *layered_leaf, permutation);
    }
    const auto stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(stats.swcc_raw_line_accesses == expected_swcc_lines);
    std::free(layered_snapshot);
    latency_sim::GlobalLatencySimulator().Configure({});
  }
  assert(Masstree::leaf_replica<replica_params>::Promote(
      *layered_leaf, layered_version, replicas));
  {
    Masstree::unlocked_tcursor<replica_params> layered_cursor(
        table.table(), lcdf::Str(layered_key.data(), layered_key.size()));
    assert(layered_cursor.find_unlocked(*ti));
    const auto column = layered_cursor.value()->col(0);
    assert(std::string(column.s, column.len) == expected.at(layered_key));
  }
  std::free(replicas.Invalidate(layered_leaf->control_ref()));

  assert(!table.table().root()->isleaf());
  auto* canonical_root = static_cast<Masstree::internode<replica_params>*>(table.table().root());
  assert(dsidle::LoadCanonicalNodeBytes(canonical_root->control_ref()) ==
         sizeof(*canonical_root));
  const auto root_replica_version = canonical_root->stable();
  assert(Masstree::internode_replica<replica_params>::Promote(*canonical_root, root_replica_version, replicas));
  const auto root_generation = canonical_root->control_ref().get(pool.base())->generation;
  auto root_replica = replicas.Acquire(canonical_root->control_ref(), root_generation,
                                       root_replica_version.version_value());
  assert(root_replica);
  const auto& root_key_text = expected.begin()->first;
  replica_key_type root_key(root_key_text.data(), root_key_text.size());
  const auto replica_child = Masstree::internode_replica<replica_params>::LookupChild(
      root_replica.snapshot().local_ptr, root_key);
  const auto canonical_child_index = Masstree::internode<replica_params>::bound_type::upper(root_key, *canonical_root);
  assert(replica_child == canonical_root->child_[canonical_child_index].ref());
  root_replica = {};
  Masstree::unlocked_tcursor<replica_params> root_replica_cursor(
      table.table(), lcdf::Str(root_key_text.data(), root_key_text.size()));
  assert(root_replica_cursor.find_unlocked(*ti));
  assert(replicas.InternalHits() > 0);
  std::free(replicas.Invalidate(canonical_root->control_ref()));
  Masstree::root_replica_pin<replica_params> root_pin(pool, replicas);
  replicas.SetBudgetBytes(0);
  assert(root_pin.Refresh(*ti));
  assert(root_pin.ref() == dsidle::RootControlAccessor(pool.root_control()).stable().ref);
  assert(replicas.LocalBytes() > 0);
  replicas.SetBudgetBytes(UINT64_MAX);

  // Encode a stable canonical leaf into local DRAM and read its copied row
  // without following its canonical ValueRef or suffix pointer.
  const auto& replica_key_text = expected.begin()->first;
  replica_key_type replica_key(replica_key_text.data(), replica_key_text.size());
  typename replica_node_type::nodeversion_type replica_version;
  auto* canonical_leaf = table.table().root()->reach_leaf(replica_key, replica_version, *ti);
  assert(replicas.AccessCount(canonical_leaf->control_ref()) > 0);
  if (latency_sim::TscSpinAvailableForTest()) {
    const int slot = find_slot(canonical_leaf, replica_key_text);
    assert(slot >= 0 && !canonical_leaf->is_layer(slot));
    latency_sim::Config prefetch_latency;
    prefetch_latency.enabled = true;
    prefetch_latency.stats_enabled = true;
    prefetch_latency.cache_line_bytes = 1;
    latency_sim::GlobalLatencySimulator().Configure(prefetch_latency);
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      canonical_leaf->lv_[slot].prefetch(canonical_leaf->keylenx_[slot]);
    }
    assert(latency_sim::GlobalLatencySimulator()
               .TakeStatsAndReset()
               .swcc_raw_line_accesses == 0);
    std::size_t value_bytes = 0;
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      const auto value = canonical_leaf->lv_[slot].value();
      value_bytes = value->size();
    }
    assert(latency_sim::GlobalLatencySimulator()
               .TakeStatsAndReset()
               .swcc_raw_line_accesses == value_bytes);
    latency_sim::GlobalLatencySimulator().Configure({});
  }
  void* encoded_leaf = Masstree::leaf_replica<replica_params>::Create(
      *canonical_leaf, canonical_leaf->permutation());
  const row_type* replica_value = nullptr;
  dsidle::NodeRef replica_layer;
  assert(Masstree::leaf_replica<replica_params>::Lookup(encoded_leaf, replica_key,
      replica_value, replica_layer) == Masstree::leaf_replica<replica_params>::result::kValue);
  const auto replica_column = replica_value->col(0);
  assert(replica_column.len == expected.begin()->second.size());
  assert(std::memcmp(replica_column.s, expected.begin()->second.data(), replica_column.len) == 0);
  std::free(encoded_leaf);
  const auto promote_version = canonical_leaf->stable();
  if (latency_sim::TscSpinAvailableForTest()) {
    latency_sim::Config promotion_latency;
    promotion_latency.enabled = true;
    promotion_latency.stats_enabled = true;
    promotion_latency.swcc_write_ns_per_line = 3;
    promotion_latency.swcc_flush_ns_per_line = 5;
    latency_sim::GlobalLatencySimulator().Configure(promotion_latency);
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kMerge);
      assert(Masstree::leaf_replica<replica_params>::Promote(
          *canonical_leaf, promote_version, replicas));
    }
    const auto promotion_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(promotion_stats.swcc_raw_line_accesses > 0);
    assert(promotion_stats.swcc_delayed_ns == 0);
    std::free(replicas.Invalidate(canonical_leaf->control_ref()));
    latency_sim::GlobalLatencySimulator().Configure({});
  }
  assert(Masstree::leaf_replica<replica_params>::Promote(*canonical_leaf, promote_version, replicas));
  const auto promote_generation = canonical_leaf->control_ref().get(pool.base())->generation;
  auto replica_handle = replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                                         promote_version.version_value());
  assert(replica_handle);
  replica_value = nullptr;
  assert(Masstree::leaf_replica<replica_params>::Lookup(replica_handle.snapshot().local_ptr,
      replica_key, replica_value, replica_layer) == Masstree::leaf_replica<replica_params>::result::kValue);
  assert(replica_value->col(0).len == expected.begin()->second.size());
  replica_handle = {};
  // Replica snapshots are optional local caches. A non-authoritative snapshot
  // miss must continue through the canonical Masstree leaf.
  auto* empty_snapshot =
      static_cast<Masstree::leaf_replica<replica_params>::header*>(
          Masstree::leaf_replica<replica_params>::Create(
              *canonical_leaf, canonical_leaf->permutation()));
  empty_snapshot->count = 0;
  std::free(replicas.Publish(
      canonical_leaf->control_ref(),
      {empty_snapshot, promote_generation, promote_version.version_value(),
       empty_snapshot->bytes, dsidle::ReplicaKind::kValueLeaf}));
  {
    Masstree::unlocked_tcursor<replica_params> fallback_cursor(
        table.table(),
        lcdf::Str(replica_key_text.data(), replica_key_text.size()));
    assert(fallback_cursor.find_unlocked(*ti));
    assert(!fallback_cursor.used_replica());
  }
  std::free(replicas.Invalidate(canonical_leaf->control_ref()));
  assert(Masstree::leaf_replica<replica_params>::Promote(
      *canonical_leaf, promote_version, replicas));
  {
    Masstree::unlocked_tcursor<replica_params> replica_cursor(
        table.table(), lcdf::Str(replica_key_text.data(), replica_key_text.size()));
    assert(replica_cursor.find_unlocked(*ti));
    assert(replica_cursor.used_replica());
    assert(replica_cursor.value()->col(0).len == expected.begin()->second.size());
  }
  if (latency_sim::TscSpinAvailableForTest()) {
    latency_sim::Config latency;
    latency.enabled = true;
    latency.stats_enabled = true;
    latency_sim::GlobalLatencySimulator().Configure(latency);
    dsidle::ReplicaDirectory empty_replicas(pool);
    dsidle::ConfigureCurrentReplicaDirectory(empty_replicas);
    bool canonical_get_used_replica = false;
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      Masstree::unlocked_tcursor<replica_params> cursor(
          table.table(),
          lcdf::Str(replica_key_text.data(), replica_key_text.size()));
      assert(cursor.find_unlocked(*ti));
      canonical_get_used_replica = cursor.used_replica();
    }
    const auto canonical_get_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(!canonical_get_used_replica);

    dsidle::ConfigureCurrentReplicaDirectory(replicas);
    bool local_get_used_replica = false;
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      Masstree::unlocked_tcursor<replica_params> cursor(
          table.table(),
          lcdf::Str(replica_key_text.data(), replica_key_text.size()));
      assert(cursor.find_unlocked(*ti));
      local_get_used_replica = cursor.used_replica();
    }
    const auto local_get_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(local_get_used_replica);
    assert(local_get_stats.swcc_raw_line_accesses == 0);
    assert(canonical_get_stats.swcc_raw_line_accesses > 0);

    dsidle::ConfigureCurrentReplicaDirectory(empty_replicas);
    ReplicaPointerScanCollector canonical_scan{
        static_cast<const std::byte*>(pool.base()),
        static_cast<const std::byte*>(pool.base()) + kPoolBytes};
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      table.table().scan(
          lcdf::Str(replica_key_text.data(), replica_key_text.size()), true,
          canonical_scan, *ti);
    }
    const auto canonical_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(!canonical_scan.used_replica);

    dsidle::ConfigureCurrentReplicaDirectory(replicas);
    ReplicaPointerScanCollector measured_replica_scan{
        static_cast<const std::byte*>(pool.base()),
        static_cast<const std::byte*>(pool.base()) + kPoolBytes};
    {
      latency_sim::ScopeGuard scope(latency_sim::ScopeKind::kForeground);
      table.table().scan(
          lcdf::Str(replica_key_text.data(), replica_key_text.size()), true,
          measured_replica_scan, *ti);
    }
    const auto replica_stats =
        latency_sim::GlobalLatencySimulator().TakeStatsAndReset();
    assert(measured_replica_scan.used_replica);
    assert(replica_stats.swcc_raw_line_accesses <
           canonical_stats.swcc_raw_line_accesses);
    latency_sim::GlobalLatencySimulator().Configure({});
  }
  ReplicaPointerScanCollector replica_scan{
      static_cast<const std::byte*>(pool.base()),
      static_cast<const std::byte*>(pool.base()) + kPoolBytes};
  table.table().scan(
      lcdf::Str(replica_key_text.data(), replica_key_text.size()), true,
      replica_scan, *ti);
  assert(replica_scan.used_replica);
  // D-SIDLE executor candidates are generation-qualified NodeRefs. It only
  // publishes/evicts local buffers and leaves the canonical node untouched.
  sidle::sidle_threshold replica_thresholds;
  Masstree::replica_executor<replica_params> replica_executor(
      replicas, replica_thresholds);
  std::free(replicas.Invalidate(canonical_leaf->control_ref()));
  std::free(replicas.Invalidate(canonical_root->control_ref()));
  assert(!replicas.Acquire(canonical_root->control_ref(), root_generation,
                           root_replica_version.version_value()));
  assert(replica_executor.Promote({canonical_leaf->control_ref(), promote_generation}, *ti));
  assert(replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                          promote_version.version_value()));
  const auto promoted_root_version = canonical_root->stable();
  assert(replicas.Acquire(canonical_root->control_ref(), root_generation,
                          promoted_root_version.version_value()));
  assert(!replica_executor.Promote({canonical_leaf->control_ref(), promote_generation + 1}, *ti));
  assert(replica_executor.Demote({canonical_leaf->control_ref(), promote_generation}, *ti));
  assert(!replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                           promote_version.version_value()));
  sidle::sidle_threshold worker_thresholds(1024, 80, false);
  worker_thresholds.set_hotness_watermarks(0);
  Masstree::replica_workers<replica_params> workers(
      table.table(), pool, replicas, worker_thresholds,
      1, 0, 1,
      std::chrono::milliseconds(1), std::chrono::hours(1),
      std::chrono::hours(1));
  const auto resident_bytes = replicas.LocalBytes();
  assert(resident_bytes > 0);
  replicas.SetBudgetBytes(resident_bytes);
  for (unsigned i = 0; i != 256; ++i)
    replicas.RecordAccess(canonical_leaf->control_ref());
  const auto cold_watermark = worker_thresholds.get_cold_watermark();
  workers.Start();
  workers.AdjustOnce();
  const auto stop_started = std::chrono::steady_clock::now();
  workers.Stop();
  assert(std::chrono::steady_clock::now() - stop_started <
         std::chrono::seconds(5));
  assert(workers.PromotionEnabled());
  assert(workers.ForcedDemotionRounds() == 0);
  // Three shortage rounds plus the original SIDLE post-loop adjustment.
  assert(worker_thresholds.get_cold_watermark() == cold_watermark + 4);
  replicas.SetBudgetBytes(UINT64_MAX);
  workers.AdjustOnce();
  assert(workers.PromotionEnabled());
  for (unsigned i = 0; i != 256; ++i) replicas.RecordAccess(canonical_leaf->control_ref());
  workers.TriggerOnce(*ti);
  workers.ExecuteOnce(sidle::task_type::promotion, *ti);
  assert(replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                          promote_version.version_value()));
  workers.CoolOnce(*ti);
  std::free(replicas.Invalidate(canonical_leaf->control_ref()));
  // Simulate a remote VM directory: A's writer must not touch it, while B's
  // subsequent lookup rejects its old version and returns the new canonical
  // value instead of a stale copied row.
  dsidle::ReplicaDirectory remote_replicas(pool);
  assert(Masstree::leaf_replica<replica_params>::Promote(*canonical_leaf, promote_version, remote_replicas));
  dsidle::ConfigureCurrentReplicaDirectory(replicas);
  const std::string put_updated = "replica-run-put";
  lcdf::Json put_change[2] = {lcdf::Json(0), lcdf::Json(put_updated)};
  assert(query.run_put(
             table.table(),
             lcdf::Str(replica_key_text.data(), replica_key_text.size()),
             put_change, put_change + 2, *ti) == Updated);
  expected[replica_key_text] = put_updated;
  dsidle::ConfigureCurrentReplicaDirectory(remote_replicas);
  {
    ReplicaPointerScanCollector stale_scan{
        static_cast<const std::byte*>(pool.base()),
        static_cast<const std::byte*>(pool.base()) + kPoolBytes};
    table.table().scan(
        lcdf::Str(replica_key_text.data(), replica_key_text.size()), true,
        stale_scan, *ti);
    assert(!stale_scan.used_replica);
    assert(remote_replicas.LocalBytes() == 0);
  }
  {
    Masstree::unlocked_tcursor<replica_params> remote_cursor(
        table.table(), lcdf::Str(replica_key_text.data(), replica_key_text.size()));
    assert(remote_cursor.find_unlocked(*ti));
    const auto column = remote_cursor.value()->col(0);
    assert(column.len == put_updated.size());
    assert(std::memcmp(column.s, put_updated.data(), column.len) == 0);
  }
  assert(remote_replicas.LocalBytes() == 0);
  const auto after_put_version = canonical_leaf->stable();
  assert(Masstree::leaf_replica<replica_params>::Promote(
      *canonical_leaf, after_put_version, remote_replicas));
  dsidle::ConfigureCurrentReplicaDirectory(replicas);
  const std::string replica_updated = "replica-local-write";
  query.run_replace(table.table(), lcdf::Str(replica_key_text.data(), replica_key_text.size()),
                    lcdf::Str(replica_updated.data(), replica_updated.size()), *ti);
  expected[replica_key_text] = replica_updated;
  assert(!replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                           promote_version.version_value()));
  dsidle::ConfigureCurrentReplicaDirectory(remote_replicas);
  {
    Masstree::unlocked_tcursor<replica_params> remote_cursor(
        table.table(), lcdf::Str(replica_key_text.data(), replica_key_text.size()));
    assert(remote_cursor.find_unlocked(*ti));
    const auto column = remote_cursor.value()->col(0);
    assert(column.len == replica_updated.size());
    assert(std::memcmp(column.s, replica_updated.data(), column.len) == 0);
  }
  // VM A did not mutate VM B's process-local slot. B's first read against the
  // newer canonical version rejects and lazily reclaims its older local copy.
  assert(!remote_replicas.Acquire(canonical_leaf->control_ref(), promote_generation,
                                  promote_version.version_value()));
  assert(remote_replicas.LocalBytes() == 0);
  dsidle::ConfigureCurrentReplicaDirectory(replicas);
  std::free(replicas.Invalidate(canonical_leaf->control_ref()));

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
  assert(root_pin.Refresh(*ti));
  assert(root_pin.ref() == dsidle::RootControlAccessor(pool.root_control()).stable().ref);

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
