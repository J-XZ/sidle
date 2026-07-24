#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

#include "masstree.hh"
#include "masstree_insert.hh"
#include "masstree_remove.hh"
#include "masstree_scan.hh"
#include "masstree_tcursor.hh"
#include "masstree_replica_worker.hh"
#include "query_masstree.hh"
#include "sidle_frontend.hh"

#include "helper.h"

#ifdef CAL_NODE_HOTNESS
#include <fstream>
#include <vector>
#endif

#if !defined(MASSTREE_H)
#define MASSTREE_H

typedef Masstree::default_table mass_tree_t;
using mass_leaf_t = Masstree::leaf<Masstree::default_query_table_params>;
using mass_internode_t = Masstree::internode<Masstree::default_query_table_params>;
using mass_node_t = Masstree::node_base<Masstree::default_query_table_params>;
using masstree_t = Masstree::basic_table<Masstree::default_query_table_params>;

template <typename K, typename V>
class MasstreeKV {
  static const size_t key_size = sizeof(K);
  static const size_t val_size = sizeof(V);

 public:
  typedef typename mass_tree_t::leaf_type leaf_type;
  typedef typename mass_tree_t::node_type node_type;
  MasstreeKV(threadinfo *main_ti, dsidle::SharedPool& pool,
             const uint64_t replica_budget_mb, const uint64_t hot_percentage_seed);
  ~MasstreeKV();
  bool get(const K &k, V &v, threadinfo *ti, query<row_type> &q,
           const uint32_t worker_id);
  bool insert(const K &k, const V &v, threadinfo *ti, query<row_type> &q,
              const uint32_t worker_id);
  bool remove(const K &k, threadinfo *ti, query<row_type> &q,
              const uint32_t worker_id);
  bool lower_bound(K &k, V &v, threadinfo *ti, query<row_type> &q,
                   const uint32_t worker_id);
  size_t scan(const K &k_start, size_t n, std::vector<std::pair<K, V>> &result,
              threadinfo *ti, query<row_type> &q, const uint32_t worker_id);
  size_t range_scan(const K &k_start, const K &k_end,
                    std::vector<std::pair<K, V>> &result, threadinfo *ti,
                    query<row_type> &q, const uint32_t worker_id);
  void worker_enter(const uint32_t worker_id);
  void worker_exit(const uint32_t worker_id);
  void stop();

  /// Starts the five D-SIDLE replica workers. Canonical nodes never migrate.
  void init_migration_worker(int bg_worker_start_tid, int basic_worker_wakeup_interval, 
    int cooler_wakeup_interval, int threshold_adjuster_wakeup_interval, uint64_t hot_percentage_lower_bound);

  /// @brief function for tpc-c
  void start_bg();
  void terminate_bg();

 private:
  mass_tree_t mass_tree;
  dsidle::SharedPool& pool_;
  std::unique_ptr<dsidle::ReplicaDirectory> replicas_;
  std::unique_ptr<Masstree::replica_workers<Masstree::default_query_table_params>> replica_workers_;
  threadinfo *main_ti;
};

template <typename K, typename V>
MasstreeKV<K, V>::MasstreeKV(threadinfo *main_ti, dsidle::SharedPool& pool,
                             const uint64_t replica_budget_mb, const uint64_t hot_percentage_seed)
    : pool_(pool), main_ti(main_ti) {
  sidle::strategy_manager = sidle::sidle_strategy(replica_budget_mb, hot_percentage_seed, false);
  node_type::strategy_manager = &sidle::strategy_manager;
  replicas_ = std::make_unique<dsidle::ReplicaDirectory>(pool_);
  replicas_->SetBudgetBytes(replica_budget_mb << 20);
  dsidle::ConfigureCurrentReplicaDirectory(*replicas_);
  mass_tree.initialize(*main_ti, hot_percentage_seed);
}

template <typename K, typename V>
void MasstreeKV<K, V>::stop() {
  if (replica_workers_) replica_workers_->Stop();
  replica_workers_.reset();
}

template <typename K, typename V>
MasstreeKV<K, V>::~MasstreeKV() {
#ifdef CAL_NODE_HOTNESS
  std::unordered_map<uint64_t, std::vector<uint16_t>> hotness;
  std::string hotness_file_name("masstree_hotness.csv");
  std::ofstream out(hotness_file_name);
  if (!out.is_open()) {
    std::cerr << "Failed to open hotness file: " << hotness_file_name << std::endl;
    return;
  }
  auto node_hotness = node_type::hotness_map_;
  for (auto & [addr, access_times] : node_hotness) {
    uint64_t page_id = addr >> 12;
    hotness[page_id].emplace_back(access_times);
  }
  std::vector<std::vector<uint16_t>> hotness_summary;
  hotness.reserve(hotness.size());
  for (auto& [page_id, access_times] : hotness) {
    uint64_t sum = std::accumulate(access_times.begin(), access_times.end(), 0);
    access_times.insert(access_times.begin(), sum);
    hotness_summary.emplace_back(access_times);
  }
  std::sort(hotness_summary.begin(), hotness_summary.end(),
            [](const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
              return a[0] > b[0];
            });
  for (const auto& access_times : hotness_summary) {
    for (const auto& access_time : access_times) {
      out << access_time << ",";
    }
    out << std::endl;
  }
  out.close();
#endif
  stop();
}

template <typename K, typename V>
bool MasstreeKV<K, V>::get(const K &k, V &v, threadinfo *ti, query<row_type> &q,
                           const uint32_t worker_id) {
  K str_k = k.to_str_key();
  Str key((char *)&str_k, key_size);
  Str val_str;
  bool got = q.run_get1(mass_tree.table(), key, 0, val_str, *ti);
  if (got) {
    try {
      v = *((V *)val_str.s);
    } catch (const std::exception &e) {
      std::cerr << "Exception in MasstreeKV::get: " << e.what() << std::endl;
    }
    return true;
  }
  return false;
}

template <typename K, typename V>
bool MasstreeKV<K, V>::insert(const K &k, const V &v, threadinfo *ti,
                              query<row_type> &q, const uint32_t worker_id) {
  K str_k = k.to_str_key();
  Str key((char *)&str_k, key_size);
  Str val((char *)&v, val_size);
 result_t res = q.run_replace(mass_tree.table(), key, val, *ti);
  assert(res == Inserted || res == Updated);
  return res == Inserted || res == Updated;
}

template <typename K, typename V>
bool MasstreeKV<K, V>::remove(const K &k, threadinfo *ti, query<row_type> &q,
                              const uint32_t worker_id) {
  K str_k = k.to_str_key();
  Str key((char *)&str_k, key_size);
  q.run_remove(mass_tree.table(), key, *ti);
  return true;
}

template <typename K, typename V>
bool MasstreeKV<K, V>::lower_bound(K &k, V &v, threadinfo *ti,
                                   query<row_type> &q,
                                   const uint32_t worker_id) {
  throw std::runtime_error("MasstreeKV::lower_bound not implemented");
  return false;
}

template <typename K, typename V>
size_t MasstreeKV<K, V>::scan(const K &k_start, size_t n,
                              std::vector<std::pair<K, V>> &result,
                              threadinfo *ti, query<row_type> &q,
                              const uint32_t worker_id) {
  K str_k = k_start.to_str_key();
  Str first_key((char *)&str_k, key_size);
  lcdf::Json req = lcdf::Json::array(0, 0, first_key, n);
  q.run_scan(mass_tree.table(), req, *ti);
  assert(req.size() >= 2);

  for (int i = 2; i < req.size(); i += 2) {
    result.emplace_back(((K *)req[i].as_s().data())->to_normal_key(),
                        *(V *)req[i + 1].as_s().data());
  }
  return result.size();
}

template <typename K, typename V>
size_t MasstreeKV<K, V>::range_scan(const K &k_start, const K &k_end,
                                    std::vector<std::pair<K, V>> &result,
                                    threadinfo *ti, query<row_type> &q,
                                    const uint32_t worker_id) {
  throw std::runtime_error("MasstreeKV::range_scan not implemented");
  return 0;
}

template <typename K, typename V>
void MasstreeKV<K, V>::worker_enter(const uint32_t worker_id) {
  (void) worker_id;
  dsidle::ConfigureCurrentReplicaDirectory(*replicas_);
}

template <typename K, typename V>
void MasstreeKV<K, V>::worker_exit(const uint32_t worker_id) {}

template <typename K, typename V>
void MasstreeKV<K, V>::init_migration_worker(int bg_worker_start_tid, int basic_worker_wakeup_interval, int cooler_wakeup_interval, int threshold_adjuster_wakeup_interval, uint64_t hot_percentage_lower_bound) {
  (void) bg_worker_start_tid;
  sidle::sidle_threshold* thresholds = sidle::strategy_manager.get_threshold_manager();
  thresholds->set_hotness_watermarks(hot_percentage_lower_bound);
  stop();
  replica_workers_ = std::make_unique<Masstree::replica_workers<Masstree::default_query_table_params>>(
      mass_tree.table(), pool_, *replicas_, *thresholds,
      std::chrono::milliseconds(basic_worker_wakeup_interval),
      std::chrono::milliseconds(cooler_wakeup_interval),
      std::chrono::milliseconds(threshold_adjuster_wakeup_interval));
  replica_workers_->Start();
}

template <typename K, typename V>
void MasstreeKV<K, V>::start_bg() {}

template <typename K, typename V>
void MasstreeKV<K, V>::terminate_bg() {}

#endif  // MASSTREE_H
