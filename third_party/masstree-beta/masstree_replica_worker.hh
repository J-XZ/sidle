#ifndef MASSTREE_REPLICA_WORKER_HH
#define MASSTREE_REPLICA_WORKER_HH

#include <unistd.h>

#include "masstree_internal_replica.hh"
#include "masstree_replica.hh"
#include "masstree_root_replica.hh"
#include "masstree_sidle.hh"
#include "sidle_meta.hh"
#include "sidle_worker.hh"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace Masstree {

// D-SIDLE executor for the promotion/demotion queues.  It deliberately never
// locks, relinks, marks deleted, or RCU-frees a canonical node: those are tree
// writer responsibilities.  Its only mutation is process-local directory
// publication/eviction after validating the queued NodeRef generation.
template <typename P>
class replica_executor {
 public:
  explicit replica_executor(dsidle::ReplicaDirectory& directory) : directory_(directory) {}

  static dsidle::QueuedNodeRef Candidate(const node_base<P>& node) {
    const auto ref = node.control_ref();
    return {ref, ref ? ref.get(dsidle::SharedPoolBase())->generation : 0};
  }

  bool Promote(dsidle::QueuedNodeRef candidate) {
    node_base<P>* node = Resolve(candidate);
    if (!node) return false;
    bool published = false;
    // Preserve SIDLE's leaf-to-ancestor promotion direction.  Stop once this
    // VM already has a valid ancestor replica, including the pinned root.
    while (node) {
      const auto version = node->stable();
      if (version.deleted() || version.locked()) return published;
      const auto ref = node->control_ref();
      const auto generation = ref.get(dsidle::SharedPoolBase())->generation;
      if (directory_.Acquire(ref, generation, version.version_value())) break;
      const bool one = node->isleaf()
          ? leaf_replica<P>::Promote(*static_cast<leaf<P>*>(node), version, directory_)
          : internode_replica<P>::Promote(*static_cast<internode<P>*>(node), version, directory_);
      if (!one) return published;
      published = true;
      node = node->parent();
    }
    return published;
  }

  bool Demote(dsidle::QueuedNodeRef candidate) {
    node_base<P>* node = Resolve(candidate);
    if (!node || node->is_root()) return false;  // root is pinned per VM.
    std::free(directory_.Invalidate(node->control_ref()));
    return true;
  }

  bool ExecuteOne(sidle::task_type type, dsidle::QueuedNodeRef candidate) {
    return type == sidle::task_type::promotion ? Promote(candidate) : Demote(candidate);
  }

 private:
  static node_base<P>* Resolve(dsidle::QueuedNodeRef candidate) {
    if (!candidate) return nullptr;
    auto* control = candidate.ref.get(dsidle::SharedPoolBase());
    if (!control || control->allocation_state != dsidle::NodeAllocationState::kPublished ||
        control->generation != candidate.generation)
      return nullptr;
    return dsidle::ResolveCanonicalNode<node_base<P>>(candidate.ref);
  }

  dsidle::ReplicaDirectory& directory_;
};

// The five original SIDLE roles, with migration actions replaced by local
// replica work.  Selection remains local to one VM; canonical traversal is
// read-only and candidates use generation-qualified NodeRefs.
template <typename P>
class replica_workers {
 public:
  using table_type = basic_table<P>;
  replica_workers(table_type& table, dsidle::SharedPool& pool,
                  dsidle::ReplicaDirectory& directory, sidle::sidle_threshold& thresholds,
                  std::chrono::milliseconds basic_interval,
                  std::chrono::milliseconds cooler_interval,
                  std::chrono::milliseconds adjuster_interval)
      : table_(table), directory_(directory), thresholds_(thresholds),
        histogram_(std::make_shared<sidle::sidle_histogram>(&thresholds)),
        root_pin_(pool, directory), basic_interval_(basic_interval),
        cooler_interval_(cooler_interval), adjuster_interval_(adjuster_interval) {}

  ~replica_workers() { Stop(); }
  replica_workers(const replica_workers&) = delete;
  replica_workers& operator=(const replica_workers&) = delete;

  void Start() {
    if (running_.exchange(true)) return;
    trigger_ = std::thread([this] { TriggerLoop(); });
    promotion_ = std::thread([this] { ExecutorLoop(sidle::task_type::promotion); });
    demotion_ = std::thread([this] { ExecutorLoop(sidle::task_type::demotion); });
    cooler_ = std::thread([this] { CoolerLoop(); });
    adjuster_ = std::thread([this] { AdjusterLoop(); });
  }
  void Stop() {
    if (!running_.exchange(false)) return;
    for (std::thread* thread : {&trigger_, &promotion_, &demotion_, &cooler_, &adjuster_})
      if (thread->joinable()) thread->join();
  }

  void TriggerOnce(threadinfo& ti) {
    bool after_cooling = false;
    while (histogram_->cooling() && running_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      after_cooling = true;
    }
    if (after_cooling) histogram_->decrease_tolerance_for_cold();
    std::uint64_t nodes = 0;
    std::uint64_t accesses = 0;
    masstree_leaf_traverse<P, threadinfo*>(&table_, [this, &nodes, &accesses](internode<P>*, leaf<P>* leaf) {
      const auto ref = leaf->control_ref();
      const auto version = leaf->stable();
      if (version.deleted() || version.locked()) return;
      const auto generation = ref.get(dsidle::SharedPoolBase())->generation;
      const auto count = directory_.AccessCount(ref);
      const auto hotness = histogram_->update(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, UINT16_MAX)));
      ++nodes;
      accesses += count;
      const bool local = static_cast<bool>(directory_.Acquire(ref, generation, version.version_value()));
      if (!local && hotness == sidle::sidle_histogram::type::hot)
        queue_.add(sidle::task_type::promotion, {ref, generation});
      else if (local && hotness == sidle::sidle_histogram::type::cold)
        queue_.add(sidle::task_type::demotion, {ref, generation});
    }, &ti);
    histogram_->refresh(nodes, accesses);
    histogram_->adjust_threshold();
    root_pin_.Refresh();
  }

  void ExecuteOnce(sidle::task_type type) {
    replica_executor<P> executor(directory_);
    dsidle::QueuedNodeRef candidate;
    while (queue_.get(type, candidate)) executor.ExecuteOne(type, candidate);
  }

  void CoolOnce(threadinfo& ti) {
    histogram_->notify_cooling();
    masstree_leaf_traverse<P, threadinfo*>(&table_, [this](internode<P>*, leaf<P>* leaf) {
      directory_.HalveAccess(leaf->control_ref());
    }, &ti);
    histogram_->adjust_for_cooling();
  }

 private:
  void TriggerLoop() { auto* ti = threadinfo::make(threadinfo::TI_MIGRATION, -1); while (running_) { TriggerOnce(*ti); std::this_thread::sleep_for(basic_interval_ * 5); } }
  void ExecutorLoop(sidle::task_type type) { while (running_) { if (queue_.length(type) > sidle::queue_waiting_threshold) ExecuteOnce(type); std::this_thread::sleep_for(basic_interval_); ExecuteOnce(type); } }
  void CoolerLoop() { auto* ti = threadinfo::make(threadinfo::TI_MIGRATION, -1); while (running_) { std::this_thread::sleep_for(cooler_interval_); CoolOnce(*ti); } }
  void AdjusterLoop() { while (running_) { std::this_thread::sleep_for(adjuster_interval_); const auto budget = directory_.BudgetBytes(); if (budget == UINT64_MAX) continue; const double ratio = static_cast<double>(directory_.LocalBytes()) / budget; if (thresholds_.check_memory_usage(ratio) == sidle::mem_usage_status::tight) { thresholds_.adjust_local_allocation_threshold(true); thresholds_.adjust_demotion_depth_threshold(true); } } }

  table_type& table_;
  dsidle::ReplicaDirectory& directory_;
  sidle::sidle_threshold& thresholds_;
  std::shared_ptr<sidle::sidle_histogram> histogram_;
  sidle::replica_queue queue_;
  root_replica_pin<P> root_pin_;
  std::chrono::milliseconds basic_interval_, cooler_interval_, adjuster_interval_;
  std::atomic<bool> running_{false};
  std::thread trigger_, promotion_, demotion_, cooler_, adjuster_;
};

}  // namespace Masstree

#endif
