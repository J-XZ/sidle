#ifndef MASSTREE_REPLICA_WORKER_HH
#define MASSTREE_REPLICA_WORKER_HH

#include <unistd.h>

#include "masstree_internal_replica.hh"
#include "masstree_replica.hh"

namespace dsidle_replica_worker_detail {

template <typename T>
inline T Load(const std::atomic<T>& value, std::memory_order order) {
  return latency_sim::FixedLatencyAtomicLoad(
      value, order, latency_sim::AtomicDomain::kLocalDram);
}

template <typename T>
inline void Store(std::atomic<T>& value, T desired, std::memory_order order) {
  latency_sim::FixedLatencyAtomicStore(
      value, desired, order, latency_sim::AtomicDomain::kLocalDram);
}

template <typename T>
inline T Exchange(std::atomic<T>& value, T desired, std::memory_order order) {
  return latency_sim::FixedLatencyAtomicExchange(
      value, desired, order, latency_sim::AtomicDomain::kLocalDram);
}

}  // namespace dsidle_replica_worker_detail
#include "masstree_root_replica.hh"
#include "masstree_scan.hh"
#include "sidle_meta.hh"
#include "sidle_worker.hh"
#include "dsidle/latency_simulator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

namespace Masstree {

// D-SIDLE executor for the promotion/demotion queues.  It deliberately never
// locks, relinks, marks deleted, or RCU-frees a canonical node: those are tree
// writer responsibilities.  Its only mutation is process-local directory
// publication/eviction after validating the queued NodeRef generation.
template <typename P>
class replica_executor {
 public:
  replica_executor(dsidle::ReplicaDirectory& directory,
                   sidle::sidle_threshold& thresholds,
                   sidle::replica_queue* queue = nullptr)
      : directory_(directory), thresholds_(thresholds), queue_(queue) {}

  static dsidle::QueuedNodeRef Candidate(const node_base<P>& node) {
    const auto ref = node.control_ref();
    return {ref, ref ? dsidle::LoadNodeGeneration(ref) : 0};
  }

  bool Promote(dsidle::QueuedNodeRef candidate, typename P::threadinfo_type& ti) {
    typename P::threadinfo_type::rcu_scope rcu(ti);
    node_base<P>* node = Resolve(candidate);
    if (!node) return false;
    bool published = false;
    // Preserve original SIDLE promotion semantics: promote the selected leaf
    // and every nonlocal ancestor. Parent links are NodeRef-backed and are
    // therefore valid at every VM mapping.
    while (node) {
      const auto version = node->stable();
      if (version.deleted() || version.locked()) return published;
      const auto ref = node->control_ref();
      if (!ref) return published;
      const auto generation = dsidle::LoadNodeGeneration(ref);
      if (directory_.Acquire(ref, generation, version.version_value())) break;
      const bool refresh =
          directory_.HasLocalPlacement(ref, generation);
      node_base<P>* parent = node->parent();
      const bool one = node->isleaf()
          ? leaf_replica<P>::Promote(
                *static_cast<leaf<P>*>(node), version, directory_, true,
                refresh)
          : internode_replica<P>::Promote(
                *static_cast<internode<P>*>(node), version, directory_, true,
                refresh);
      if (!one) return published;
      published = true;
      node = parent;
    }
    return published;
  }

  bool Demote(dsidle::QueuedNodeRef candidate, typename P::threadinfo_type& ti) {
    typename P::threadinfo_type::rcu_scope rcu(ti);
    node_base<P>* node = Resolve(candidate);
    if (!node) return false;
    const auto global_root =
        dsidle::RootControlAccessor(
            dsidle::CurrentSharedPool().root_control()).stable().ref;
    if (node->control_ref() == global_root)
      return false;  // Only the global root is pinned per VM.
    bool demoted = false;
    int has_demoted = 0;
    while (node && node->control_ref() != global_root) {
      const auto version = node->stable();
      if (version.deleted() || version.locked()) break;
      const auto ref = node->control_ref();
      const auto generation = dsidle::LoadNodeGeneration(ref);
      if (!directory_.HasLocalPlacement(ref, generation))
        break;
      if (!node->isleaf()) {
        auto* internal = static_cast<internode<P>*>(node);
        if (internal->sidle_meta.depth <=
                thresholds_.get_demotion_depth_threshold() ||
            internal->sidle_meta.depth == 1)
          break;
        if (HasLocalChild(*internal)) {
          if (has_demoted && queue_)
            queue_->add(sidle::task_type::demotion,
                        Candidate(*internal));
          break;
        }
      }
      node_base<P>* parent = node->parent();
      std::free(directory_.Invalidate(ref));
      demoted = true;
      ++has_demoted;
      node = parent;
    }
    return demoted;
  }

  bool ExecuteOne(sidle::task_type type, dsidle::QueuedNodeRef candidate,
                  typename P::threadinfo_type& ti) {
    return type == sidle::task_type::promotion
        ? Promote(candidate, ti)
        : Demote(candidate, ti);
  }

 private:
  bool HasLocalChild(internode<P>& node) {
    const auto version = node.stable();
    const int count = node.size();
    dsidle::NodeRef children[internode<P>::width + 1];
    for (int index = 0; index <= count; ++index)
      children[index] = node.child_[index].ref();
    if (node.has_changed(version))
      return true;
    for (int index = 0; index <= count; ++index) {
      if (!children[index])
        continue;
      const auto generation =
          dsidle::LoadNodeGeneration(children[index]);
      if (directory_.HasLocalPlacement(children[index], generation))
        return true;
    }
    return false;
  }

  static node_base<P>* Resolve(dsidle::QueuedNodeRef candidate) {
    if (!candidate) return nullptr;
    if (dsidle::LoadNodeAllocationState(candidate.ref) !=
            dsidle::NodeAllocationState::kPublished ||
        dsidle::LoadNodeGeneration(candidate.ref) != candidate.generation)
      return nullptr;
    return dsidle::ResolveCanonicalNode<node_base<P>>(candidate.ref);
  }

  dsidle::ReplicaDirectory& directory_;
  sidle::sidle_threshold& thresholds_;
  sidle::replica_queue* queue_;
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
                  std::uint32_t shard_count, std::uint32_t local_shard,
                  std::uint32_t background_thread_base,
                  std::chrono::milliseconds basic_interval,
                  std::chrono::milliseconds cooler_interval,
                  std::chrono::milliseconds adjuster_interval)
      : table_(table), pool_(pool), directory_(directory), thresholds_(thresholds),
        histogram_(std::make_shared<sidle::sidle_histogram>(&thresholds)),
        root_pin_(pool, directory), shard_count_(shard_count), local_shard_(local_shard), background_thread_base_(background_thread_base), basic_interval_(basic_interval),
        cooler_interval_(cooler_interval), adjuster_interval_(adjuster_interval) {}

  ~replica_workers() { Stop(); }
  replica_workers(const replica_workers&) = delete;
  replica_workers& operator=(const replica_workers&) = delete;

  bool PinRoot(threadinfo& ti) {
    latency_sim::ScopeGuard latency_scope(latency_sim::ScopeKind::kMerge);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
      if (root_pin_.Refresh(ti))
        return true;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  void Start() {
    if (dsidle_replica_worker_detail::Exchange(
            running_, true, std::memory_order_seq_cst)) return;
    trigger_ = std::thread([this] { TriggerLoop(); });
    promotion_ = std::thread([this] { ExecutorLoop(sidle::task_type::promotion); });
    demotion_ = std::thread([this] { ExecutorLoop(sidle::task_type::demotion); });
    cooler_ = std::thread([this] { CoolerLoop(); });
    adjuster_ = std::thread([this] { AdjusterLoop(); });
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      if (!dsidle_replica_worker_detail::Exchange(
              running_, false, std::memory_order_seq_cst))
        return;
    }
    worker_wakeup_.notify_all();
    for (std::thread* thread : {&trigger_, &promotion_, &demotion_, &cooler_, &adjuster_})
      if (thread->joinable()) thread->join();
  }

  void TriggerOnce(threadinfo& ti, bool forced_demotion = false) {
    latency_sim::ScopeGuard latency_scope(latency_sim::ScopeKind::kMerge);
    if (forced_demotion)
      dsidle_replica_worker_detail::Store(
          called_by_adjuster_, true, std::memory_order_relaxed);
    bool after_cooling = false;
    while (histogram_->cooling() && dsidle_replica_worker_detail::Load(
                                         running_, std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      after_cooling = true;
    }
    if (dsidle_replica_worker_detail::Load(
            called_by_adjuster_, std::memory_order_relaxed) && after_cooling)
      histogram_->decrease_tolerance_for_cold();
    root_pin_.Refresh(ti);
    std::uint64_t nodes = 0;
    std::uint64_t accesses = 0;
    {
      threadinfo::rcu_scope rcu(ti);
      ForEachLeaf(table_.root(), ti,
                  [this, &nodes, &accesses, forced_demotion](leaf<P>* leaf) {
        const auto ref = leaf->control_ref();
        const auto version = leaf->stable();
        if (version.deleted() || version.locked()) return;
        const auto generation = dsidle::LoadNodeGeneration(ref);
        const auto count = directory_.AccessCount(ref);
        const auto hotness = histogram_->update(static_cast<std::uint16_t>(std::min<std::uint64_t>(count, UINT16_MAX)));
        ++nodes;
        accesses += count;
        const bool desired =
            directory_.HasLocalPlacement(ref, generation);
        const bool valid = static_cast<bool>(
            directory_.Acquire(
                ref, generation, version.version_value()));
        if (dsidle_replica_worker_detail::Load(
                can_promote_, std::memory_order_relaxed) &&
            !valid && hotness == sidle::sidle_histogram::type::hot)
          queue_.add(sidle::task_type::promotion, {ref, generation});
        else if (hotness == sidle::sidle_histogram::type::cold &&
                 (desired || forced_demotion))
          queue_.add(sidle::task_type::demotion, {ref, generation});
      });
    }
    histogram_->refresh(nodes, accesses);
    histogram_->adjust_threshold();
  }

  void ExecuteOnce(sidle::task_type type, threadinfo& ti) {
    latency_sim::ScopeGuard latency_scope(latency_sim::ScopeKind::kMerge);
    if (type == sidle::task_type::promotion &&
        !dsidle_replica_worker_detail::Load(
            can_promote_, std::memory_order_relaxed)) {
      dsidle::QueuedNodeRef discarded;
      while (queue_.get(type, discarded)) {}
      return;
    }
    replica_executor<P> executor(directory_, thresholds_, &queue_);
    dsidle::QueuedNodeRef candidate;
    while (queue_.get(type, candidate)) executor.ExecuteOne(type, candidate, ti);
  }

  void CoolOnce(threadinfo& ti) {
    latency_sim::ScopeGuard latency_scope(latency_sim::ScopeKind::kMerge);
    histogram_->notify_cooling();
    {
      threadinfo::rcu_scope rcu(ti);
      ForEachLeaf(table_.root(), ti, [this](leaf<P>* leaf) {
        directory_.HalveAccess(leaf->control_ref());
      });
    }
    histogram_->adjust_for_cooling();
  }

  void AdjustOnce() { AdjustOnceImpl(); }

  bool PromotionEnabled() const {
    return dsidle_replica_worker_detail::Load(
        can_promote_, std::memory_order_relaxed);
  }
  unsigned ForcedDemotionRounds() const {
    return dsidle_replica_worker_detail::Load(
        forced_demotion_rounds_, std::memory_order_relaxed);
  }
 private:
  sidle::mem_usage_status MemoryStatus() const {
    const auto budget = directory_.BudgetBytes();
    const auto local = directory_.LocalBytes();
    const double ratio =
        budget ? static_cast<double>(local) / static_cast<double>(budget)
               : (local ? 1.0 : 0.0);
    return thresholds_.check_memory_usage(ratio);
  }

  bool RunForcedDemotionRound() {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    if (!dsidle_replica_worker_detail::Load(
            running_, std::memory_order_relaxed))
      return false;
    const std::uint64_t requested = ++requested_adjustment_round_;
    worker_wakeup_.notify_all();
    worker_wakeup_.wait(lock, [this, requested] {
      return !dsidle_replica_worker_detail::Load(
                 running_, std::memory_order_relaxed) ||
             completed_adjustment_round_ >= requested;
    });
    return completed_adjustment_round_ >= requested;
  }

  void AdjustOnceImpl() {
    std::lock_guard<std::mutex> adjuster_lock(adjuster_mutex_);
    // Preserve SIDLE's shortage handshake without adding another role:
    // threshold changes exclude an in-flight trigger/demotion, and each forced
    // trigger is acknowledged only after the existing demotion executor drains
    // its queue. The adjuster can then recheck the real local-byte budget.
    const bool coordinated = dsidle_replica_worker_detail::Load(
        running_, std::memory_order_relaxed);
    if (coordinated) {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_wakeup_.wait(lock, [this] {
        return !dsidle_replica_worker_detail::Load(
                   running_, std::memory_order_relaxed) ||
               (!trigger_active_ && !demotion_active_);
      });
      if (!dsidle_replica_worker_detail::Load(
              running_, std::memory_order_relaxed))
        return;
      adjustment_active_ = true;
    }

    const auto finish_adjustment = [this, coordinated] {
      dsidle_replica_worker_detail::Store(
          forced_demotion_rounds_, 0u, std::memory_order_relaxed);
      dsidle_replica_worker_detail::Store(
          can_promote_, true, std::memory_order_relaxed);
      if (coordinated) {
        {
          std::lock_guard<std::mutex> lock(worker_mutex_);
          adjustment_active_ = false;
        }
        worker_wakeup_.notify_all();
      }
    };

    auto status = MemoryStatus();
    if (status == sidle::mem_usage_status::tight) {
      dsidle_replica_worker_detail::Store(
          can_promote_, false, std::memory_order_relaxed);
      thresholds_.adjust_local_allocation_threshold(true);
      thresholds_.adjust_demotion_depth_threshold(true);
      dsidle_replica_worker_detail::Store(
          forced_demotion_rounds_,
          static_cast<unsigned>(sidle::default_threshold_adjust_times),
          std::memory_order_relaxed);
      for (unsigned round = 0;
           round != sidle::default_threshold_adjust_times; ++round) {
        thresholds_.adjust_hotness_watermark(false, false);
        histogram_->adjust_threshold();
        if (!RunForcedDemotionRound())
          break;
        dsidle_replica_worker_detail::Store(
            forced_demotion_rounds_,
            static_cast<unsigned>(sidle::default_threshold_adjust_times -
                                  round - 1),
            std::memory_order_relaxed);
        status = MemoryStatus();
        if (status != sidle::mem_usage_status::tight)
          break;
      }
      thresholds_.adjust_local_allocation_threshold(false);
      thresholds_.adjust_demotion_depth_threshold(false);
      thresholds_.adjust_hotness_watermark(false, false);
    } else if (status == sidle::mem_usage_status::sufficient) {
      thresholds_.adjust_local_allocation_threshold(false);
      thresholds_.adjust_demotion_depth_threshold(false);
      thresholds_.adjust_hotness_watermark(false, true);
    } else {
      thresholds_.adjust_hotness_watermark(false, false);
    }
    finish_adjustment();
  }

  template <typename Callback>
  static void ForEachLeaf(node_base<P>* root, threadinfo& ti,
                          Callback&& callback) {
    if (!root) return;
    // Reuse SIDLE's Masstree leaf iterator. Unlike a child-only B-tree DFS,
    // its scan stack follows layer values as well as internal-node edges.
    leaf_iterator<P> iterator(root);
    std::uint64_t first_ikey = 0;
    iterator.init(
        Str(reinterpret_cast<const char*>(&first_ikey), sizeof(first_ikey)),
        ti);
    while (iterator.state() != scanstackelt<P>::scan_end) {
      if (auto* current = iterator.node())
        callback(current);
      iterator.next(ti);
    }
  }

  void BindCurrentThread() { dsidle::ConfigureCurrentSwccAllocator(pool_, shard_count_, local_shard_); dsidle::ConfigureCurrentReplicaDirectory(directory_); }
  bool WaitForInterval(std::chrono::milliseconds interval) {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_wakeup_.wait_for(lock, interval, [this] {
      return !dsidle_replica_worker_detail::Load(
          running_, std::memory_order_relaxed);
    });
    return dsidle_replica_worker_detail::Load(
        running_, std::memory_order_relaxed);
  }
  void ExecuteTrackedDemotion(threadinfo& ti) {
    ExecuteOnce(sidle::task_type::demotion, ti);
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      demotion_active_ = false;
    }
    worker_wakeup_.notify_all();
  }
  void TriggerLoop() {
    BindCurrentThread();
    auto* ti = threadinfo::make(
        threadinfo::TI_MIGRATION, background_thread_base_);
    bool first = true;
    while (dsidle_replica_worker_detail::Load(
        running_, std::memory_order_relaxed)) {
      std::uint64_t adjustment_round = 0;
      bool forced_demotion = false;
      {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        if (!first) {
          worker_wakeup_.wait_for(lock, basic_interval_ * 5, [this] {
            return !dsidle_replica_worker_detail::Load(
                       running_, std::memory_order_relaxed) ||
                   (adjustment_active_ &&
                    requested_adjustment_round_ >
                        triggered_adjustment_round_);
          });
        }
        if (!dsidle_replica_worker_detail::Load(
                running_, std::memory_order_relaxed))
          break;
        if (adjustment_active_) {
          if (requested_adjustment_round_ <= triggered_adjustment_round_) {
            first = false;
            continue;
          }
          adjustment_round = requested_adjustment_round_;
          forced_demotion = true;
        }
        trigger_active_ = true;
      }
      TriggerOnce(*ti, forced_demotion);
      {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        trigger_active_ = false;
        if (forced_demotion)
          triggered_adjustment_round_ = adjustment_round;
      }
      worker_wakeup_.notify_all();
      first = false;
    }
  }
  void ExecutorLoop(sidle::task_type type) {
    BindCurrentThread();
    const auto slot = background_thread_base_ +
        (type == sidle::task_type::promotion ? 1 : 2);
    auto* ti = threadinfo::make(threadinfo::TI_MIGRATION, slot);
    while (dsidle_replica_worker_detail::Load(
        running_, std::memory_order_relaxed)) {
      if (queue_.length(type) > sidle::queue_waiting_threshold) {
        bool execute_now = true;
        if (type == sidle::task_type::demotion) {
          std::lock_guard<std::mutex> lock(worker_mutex_);
          execute_now =
              dsidle_replica_worker_detail::Load(
                  running_, std::memory_order_relaxed) &&
              !adjustment_active_;
          if (execute_now)
            demotion_active_ = true;
        }
        if (execute_now) {
          if (type == sidle::task_type::demotion)
            ExecuteTrackedDemotion(*ti);
          else
            ExecuteOnce(type, *ti);
        }
      }
      std::uint64_t adjustment_round = 0;
      bool execute_demotion = false;
      if (type == sidle::task_type::demotion) {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        worker_wakeup_.wait_for(lock, basic_interval_, [this] {
          return !dsidle_replica_worker_detail::Load(
                     running_, std::memory_order_relaxed) ||
                 triggered_adjustment_round_ >
                     completed_adjustment_round_;
        });
        adjustment_round = triggered_adjustment_round_;
        execute_demotion =
            dsidle_replica_worker_detail::Load(
                running_, std::memory_order_relaxed) &&
            (!adjustment_active_ ||
             adjustment_round > completed_adjustment_round_);
        if (execute_demotion)
          demotion_active_ = true;
      } else if (!WaitForInterval(basic_interval_)) {
        break;
      }
      if (!dsidle_replica_worker_detail::Load(
              running_, std::memory_order_relaxed)) {
        if (type == sidle::task_type::demotion && execute_demotion) {
          {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            demotion_active_ = false;
          }
          worker_wakeup_.notify_all();
        }
        break;
      }
      if (type == sidle::task_type::demotion && !execute_demotion)
        continue;
      if (type == sidle::task_type::demotion)
        ExecuteTrackedDemotion(*ti);
      else
        ExecuteOnce(type, *ti);
      if (type == sidle::task_type::demotion && adjustment_round) {
        {
          std::lock_guard<std::mutex> lock(worker_mutex_);
          if (completed_adjustment_round_ < adjustment_round)
            completed_adjustment_round_ = adjustment_round;
        }
        worker_wakeup_.notify_all();
      }
    }
  }
  void CoolerLoop() {
    BindCurrentThread();
    auto* ti = threadinfo::make(
        threadinfo::TI_MIGRATION, background_thread_base_ + 3);
    while (WaitForInterval(cooler_interval_))
      CoolOnce(*ti);
  }
  void AdjusterLoop() {
    BindCurrentThread();
    while (WaitForInterval(adjuster_interval_))
      AdjustOnce();
  }

  table_type& table_;
  dsidle::SharedPool& pool_;
  dsidle::ReplicaDirectory& directory_;
  sidle::sidle_threshold& thresholds_;
  std::shared_ptr<sidle::sidle_histogram> histogram_;
  sidle::replica_queue queue_;
  root_replica_pin<P> root_pin_;
  std::uint32_t shard_count_, local_shard_, background_thread_base_;
  std::chrono::milliseconds basic_interval_, cooler_interval_, adjuster_interval_;
  std::atomic<bool> running_{false};
  std::atomic<bool> can_promote_{true};
  std::atomic<bool> called_by_adjuster_{false};
  std::atomic<unsigned> forced_demotion_rounds_{0};
  std::mutex adjuster_mutex_;
  std::mutex worker_mutex_;
  std::condition_variable worker_wakeup_;
  bool adjustment_active_{false};
  bool trigger_active_{false};
  bool demotion_active_{false};
  std::uint64_t requested_adjustment_round_{0};
  std::uint64_t triggered_adjustment_round_{0};
  std::uint64_t completed_adjustment_round_{0};
  std::thread trigger_, promotion_, demotion_, cooler_, adjuster_;
};

}  // namespace Masstree

#endif
