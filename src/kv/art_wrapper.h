#ifndef DSIDLE_ENABLE_ARCHIVED_UPSTREAM_BENCHMARK_SOURCE
#error "src/kv/art_wrapper.h is archived upstream code and is not supported by D-SIDLE"
#else

#if !defined(ART_H)
#define ART_H

#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "helper.h"
#include "sidle_worker.hh"

// #define CAL_NODE_HOTNESS

#ifdef CAL_NODE_HOTNESS
#include <fstream>
#include <unordered_map>
#endif

template <typename K, typename V>
class ARTKV {
 using histogram_ptr_t = std::shared_ptr<sidle::sidle_histogram>;
 using worker_base_ptr_t = std::shared_ptr<sidle::art_worker_base>;
 using migration_trigger_t = sidle::art_migration_trigger<art::leaf_node, art::art_node, art::adaptive_radix_tree, art::art_node>;
 using promotion_executor_t = sidle::art_promotion_executor<art::leaf_node, art::art_node, art::adaptive_radix_tree, art::art_node>;
 using demotion_executor_t = sidle::art_demotion_executor<art::leaf_node, art::art_node, art::adaptive_radix_tree, art::art_node>;
 using threshold_adjuster_t = sidle::art_threshold_adjuster;
 using cooler_t = sidle::art_cooler<art::leaf_node, art::art_node, art::adaptive_radix_tree, art::art_node>;
 using tree_op_t = sidle::tree_op<art::leaf_node, art::art_node, art::adaptive_radix_tree, art::art_node>;
 
 public:
  ARTKV(const int cxl_percentage = 0, const uint64_t max_local_memory_usage = 110);
  ~ARTKV();

  bool get(const K &k, V &v, threadinfo *ti, query<row_type> &q,
           const uint32_t worker_id);
  bool insert(const K &k, const V &v, threadinfo *ti, query<row_type> &q,
              const uint32_t worker_id);
  bool remove(const K &k, threadinfo *ti, query<row_type> &q,
              const uint32_t worker_id);
  bool lower_bound(K& /* k */, V& /* v */, threadinfo* /* ti */, query<row_type>& /* q */,
                   const uint32_t /* worker_id */) {
    return false;
  }
  size_t scan(const K& /* k_start */, size_t /* n */, std::vector<std::pair<K, V>>& /* result */,
              threadinfo* /* ti */, query<row_type>& /* q */, const uint32_t /* worker_id */) {
    return 0;
  }
  size_t range_scan(const K& /* k_start */, const K& /* k_end */,
                    std::vector<std::pair<K, V>>& /* result */, threadinfo* /* ti */,
                    query<row_type>& /* q */, const uint32_t /* worker_id */) {
    return 0;
  }
  void worker_enter(const uint32_t /* worker_id */) {};
  void worker_exit(const uint32_t /* worker_id */ ) {};

  /// @brief init the background workers to do the job related to migration and
  // and threshold adjustment
  void init_migration_worker(int bg_worker_start_tid, int basic_worker_wakeup_interval,
      int cooler_wakeup_interval, int threshold_adjuster_wakeup_interval);

  void stop();

 private:
  static const size_t key_size = sizeof(K);
  art::adaptive_radix_tree *tree;
  std::vector<worker_base_ptr_t> background_workers;
  std::vector<std::thread> background_jobs;
#ifdef CAL_NODE_HOTNESS
  std::string hotness_file_name{"art_hotness.csv"};
#endif
};

template <typename K, typename V>
ARTKV<K, V>::ARTKV(const int cxl_percentage, const uint64_t max_local_memory_usage):
  tree(art::new_adaptive_radix_tree(cxl_percentage, max_local_memory_usage)) {
    printf("[DEBUG] max local memory usage: %ld\n", max_local_memory_usage);
}

template <typename K, typename V>
ARTKV<K, V>::~ARTKV() {
  stop();
#ifdef CAL_NODE_HOTNESS
  std::unordered_map<uint64_t, std::vector<uint32_t>> hotness;
  art::adaptive_radix_tree_traverse(tree, [&](art::art_node* parent, art::leaf_node* cur) {
    if (cur != nullptr) {
      uint64_t page_id = reinterpret_cast<uint64_t>(cur) >> 12;
      hotness[page_id].emplace_back(cur->sidle_meta.access_time);
    } else {
      uint64_t page_id = reinterpret_cast<uint64_t>(parent) >> 12;
      hotness[page_id].emplace_back(parent->access_count);
    }
  }, true);
  std::ofstream out(hotness_file_name);
  if (!out.is_open()) {
    std::cerr << "Failed to open file " << hotness_file_name << std::endl;
    exit(1);
  }
  std::vector<std::vector<uint32_t>> hotness_summary;
  hotness.reserve(hotness.size());
  for (auto& [page_id, access_times] : hotness) {
    uint64_t sum = std::accumulate(access_times.begin(), access_times.end(), 0);
    access_times.insert(access_times.begin(), sum);
    hotness_summary.emplace_back(access_times);
  }
  std::sort(hotness_summary.begin(), hotness_summary.end(),
            [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
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
  delete tree;
}

template <typename K, typename V>
bool ARTKV<K, V>::get(const K &k, V &v, threadinfo* /* ti */, query<row_type>& /* q */,
           const uint32_t /* worker_id */) {
  K str_k = k.to_str_key();
  void *value = art::adaptive_radix_tree_get(tree, (char *)&str_k, key_size);
  if (value == nullptr) {
    return false;
  }
  v = *((V *)value);
  return true;
}

template <typename K, typename V>
bool ARTKV<K, V>::insert(const K &k, const V& /* v */, threadinfo* /* ti */,
                              query<row_type>& /* q */, const uint32_t /* worker_id */) {
  K str_k = k.to_str_key();
  int ret = art::adaptive_radix_tree_put(tree, (char *)&str_k, key_size);
  if (ret == 0 || ret == 1) {
    return true;
  }
  return false;
}

template <typename K, typename V>
bool ARTKV<K, V>::remove(const K & /* k */, threadinfo* /* ti */, query<row_type>& /* q */,
                              const uint32_t /* worker_id */) {
  return false;
}

template <typename K, typename V>
void ARTKV<K, V>::init_migration_worker(int bg_worker_start_tid, 
                  int basic_worker_wakeup_interval, int cooler_wakeup_interval, 
                  int threshold_adjuster_wakeup_interval) {
  printf("[DEBUG] basic_worker_wakeup_interval: %d, cooler_wakeup_interval: %d, threshold_adjuster_wakeup_interval: %d\n", basic_worker_wakeup_interval, cooler_wakeup_interval, threshold_adjuster_wakeup_interval);
  basic_worker_wakeup_interval = basic_worker_wakeup_interval * 2;
  cooler_wakeup_interval = cooler_wakeup_interval * 3;
#ifndef CAL_NODE_HOTNESS
  sidle::sidle_threshold* thresholds = 
                                  sidle::strategy_manager.get_threshold_manager();
  histogram_ptr_t histogram = std::make_shared<sidle::sidle_histogram>(thresholds);
  tree_op_t art_ops = tree_op_t{
    .traverse_func_ = art::adaptive_radix_tree_traverse,
    .leaf_migration_ = art::leaf_migration,
    .internode_migration_ = art::internode_migration,
    .unlock_ = art::unlock_node,
    .node_traverse_func_ = art::art_node_traverse,
    .get_leaf_ = art::get_art_leaf,
    .is_leaf_ = art::is_art_leaf,
    .type_ = sidle::tree_type::art,
  };
  int worker_count = 5;
  background_workers.resize(worker_count, nullptr);
  background_workers[0] = std::make_shared<migration_trigger_t>(
      std::chrono::milliseconds(basic_worker_wakeup_interval * 5), tree, histogram, art_ops);
  background_workers[1] = std::make_shared<promotion_executor_t>(
          thresholds, std::chrono::milliseconds(basic_worker_wakeup_interval),
          art_ops);
  background_workers[2] = std::make_shared<demotion_executor_t>
          (thresholds, std::chrono::milliseconds(basic_worker_wakeup_interval), art_ops);
  background_workers[3] = std::make_shared<cooler_t>(
            std::chrono::milliseconds(cooler_wakeup_interval * 5), tree, histogram, art_ops);
  background_workers[4] = std::make_shared<threshold_adjuster_t>(
            std::chrono::milliseconds(threshold_adjuster_wakeup_interval), histogram);
  background_jobs.reserve(worker_count);
  art::init_thread_pool(bg_worker_start_tid + worker_count);
  for (int i = 0; i < worker_count; ++i) {
    if (background_workers[i] == nullptr) {
      continue;
    }
    background_jobs.emplace_back(std::thread([&, i, bg_worker_start_tid]() {
      try {
        printf("[DEBUG] background worker %d start, tid: %d\n", i, 
              bg_worker_start_tid + i);
        bind_to_physical_cores(bg_worker_start_tid + i);
        background_workers[i]->run_job();
      } catch (const std::exception& e) {
        fprintf(stderr, "Exception in background worker %d: %s\n", i, e.what());
      }
    }));
  }
#endif
}

template <typename K, typename V>
void ARTKV<K, V>::stop() {
  for (auto &worker : background_workers) {
    if (worker != nullptr) {
      worker->stop();
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  for (auto &job : background_jobs) {
    job.join();
  }
  background_jobs.clear();
  background_workers.clear();
}

#endif  // ART_H

#endif  // DSIDLE_ENABLE_ARCHIVED_UPSTREAM_BENCHMARK_SOURCE
