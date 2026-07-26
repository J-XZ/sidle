#ifndef SIDLE_POLICY_HH
#define SIDLE_POLICY_HH

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "sidle_meta.hh"
#include "art_def.hh"
#include "masstree.hh"
#include "query_masstree.hh"

// #define POLICY_DEBUG

namespace art {
  struct art_node;
  struct art_node4;
  struct art_node16;
  struct art_node48;
  struct art_node256;
} // namespace art

namespace sidle {

/// @brief simplify the policy implementation in masstree,
//  But overall, the logic is repeated with masstree, and this 
// ugly coding style needs to be fix later

// migration related global config
constexpr uint64_t default_max_local_memory_usage = 32;
constexpr uint8_t max_local_depth = 8;
constexpr uint8_t estimated_child_max_count = 256;
constexpr uint8_t estimated_child_min_count = 4;
constexpr double default_upper_watermark = 0.95;
constexpr double default_lower_watermark = 0.85;
constexpr uint8_t default_hot_watermark = 5;
constexpr uint8_t default_cold_watermark = 80;
constexpr uint16_t default_hot_threshold = 128;
constexpr uint16_t default_cold_threshold = 4;
constexpr size_t queue_waiting_threshold = 10;
constexpr int default_threshold_adjust_times = 3;

/// @note this class should be singleton
class sidle_threshold {
  void set_demotion_depth_threshold() {
    demotion_depth_threshold_ = local_allocation_layer_threshold_ > 0 ?
                              local_allocation_layer_threshold_ - 1 : 0;
    default_demotion_depth_threshold_ = demotion_depth_threshold_ > 1 ? 
                          demotion_depth_threshold_ - 1 : 0;
  }

  uint8_t get_art_local_allocation_threshold(uint64_t max_local_node_count) {
    double layer_lower_bound = log2(max_local_node_count) / 
          log2(estimated_child_max_count);
    double layer_upper_bound = log2(max_local_node_count) / 
        log2(estimated_child_min_count);
    uint8_t local_allocation_layer_threshold = (layer_lower_bound + layer_upper_bound) / 2;
    // printf("[DEBUG] [art] layer lower bound: %f, layer upper bound: %f, local_allocation_layer_threshold: %u\n",
    //       layer_lower_bound, layer_upper_bound, local_allocation_layer_threshold);
    return local_allocation_layer_threshold;
  }

  uint8_t get_masstree_local_allocation_threshold(uint64_t max_local_node_count) {
    using p = Masstree::migrationparams;
    double layer_lower_bound = log2(max_local_node_count) / log2(p::leaf_width);
    double layer_upper_bound = log2((max_local_node_count + 1) / 2) / log2(p::leaf_width / 2) + 1;
    uint8_t local_allocation_layer_threshold = (layer_lower_bound + layer_upper_bound) / 2;
    // printf("[DEBUG] [Masstree] layer lower bound: %f, layer upper bound: %f, local_allocation_layer_threshold: %u\n",
    //       layer_lower_bound, layer_upper_bound, local_allocation_layer_threshold);
    return local_allocation_layer_threshold; 
  }

public:
  sidle_threshold(): local_allocation_layer_threshold_(max_local_depth) {
    set_demotion_depth_threshold();
  }

  explicit sidle_threshold(uint64_t max_local_node_count, 
                          int cxl_percentage = 80, bool is_art = true) {
    if (is_art) {
      local_allocation_layer_threshold_ = get_art_local_allocation_threshold(max_local_node_count);
    } else {
      local_allocation_layer_threshold_ = get_masstree_local_allocation_threshold(max_local_node_count);
    }
    set_demotion_depth_threshold();
    // set hot watermark according to the cxl_percentage
    hot_watermark_ = (100 - cxl_percentage) / 5;
    original_hot_watermark_ = hot_watermark_;
    hot_watermark_upper_bound_ = std::min(hot_watermark_ + hot_watermark_,
                                    100 - cxl_percentage);
    cold_watermark_ = std::min(cxl_percentage, 100 - hot_watermark_upper_bound_);
    original_cold_watermark_ = cold_watermark_;
    cold_watermark_upper_bound_ = std::min(cold_watermark_ + hot_watermark_,
                                    100 - hot_watermark_upper_bound_);
    // printf("[DEBUG] hot watermark: %u, hot watermark upper bound: %u, \
    //         cold watermark: %u, cold watermark upper bound: %u\n",
    //         hot_watermark_, hot_watermark_upper_bound_, 
    //         cold_watermark_, cold_watermark_upper_bound_);
  }

  ~sidle_threshold() = default;

  inline uint8_t get_hot_watermark() const { return hot_watermark_; }
  inline uint8_t get_cold_watermark() const { return cold_watermark_; }

  inline void set_hotness_watermarks(uint64_t hot_watermark_lower_bound) {
    hot_watermark_ = hot_watermark_lower_bound;
    original_hot_watermark_ = hot_watermark_lower_bound;
    hot_watermark_upper_bound_ = hot_watermark_ + 5;
    cold_watermark_ = hot_watermark_ + 15 < 100 ? 100 - hot_watermark_ - 15 : 100 - hot_watermark_;
    original_cold_watermark_ = cold_watermark_;
    cold_watermark_upper_bound_ = cold_watermark_ + 10;
    // printf("[DEBUG] hot watermark: %lu, cold watermark: %lu, hot watermark upper bound: %lu, cold watermark upper bound: %lu\n",
    //        hot_watermark_, cold_watermark_, hot_watermark_upper_bound_, cold_watermark_upper_bound_);
  }

  inline uint8_t get_demotion_depth_threshold() const {
    return demotion_depth_threshold_;
  }
  inline uint8_t get_local_allocation_layer_threshold() const {
    return local_allocation_layer_threshold_;
  }

  inline node_mem_type get_allocation_position(uint8_t cur_depth) {
    if (cur_depth <= local_allocation_layer_threshold_) {
      return node_mem_type::local;
    } else {
      return node_mem_type::remote;
    }
  }

  inline void correct_threshold_with_leaf_depth(uint8_t leaf_depth) {
    local_allocation_layer_threshold_ = 
      std::min(local_allocation_layer_threshold_, leaf_depth);
    demotion_depth_threshold_ = std::min(demotion_depth_threshold_, 
                                static_cast<uint8_t>(leaf_depth - 1));
  }

  inline mem_usage_status check_memory_usage(double local_mem_usage_ratio) {
    if (local_mem_usage_ratio > upper_watermark_) {
      return mem_usage_status::tight;
    } 
    if (local_mem_usage_ratio < lower_watermark_) {
      return mem_usage_status::sufficient;
    }
    return mem_usage_status::normal;
  }

  inline void adjust_local_allocation_threshold(bool is_shrink) {
    /// @note the root node of the entire tree should always in local memory
    if (is_shrink) {
      local_allocation_layer_threshold_ = local_allocation_layer_threshold_ > 1 ?
        local_allocation_layer_threshold_ - 1 : 1;
    } else {
      local_allocation_layer_threshold_ = 
          std::min(static_cast<uint8_t>(local_allocation_layer_threshold_ + 1),
              max_local_depth);
      
    }
  }

  inline void adjust_hotness_watermark(bool is_restore, bool hotter) {
    if (is_restore) {
      hot_watermark_ = original_hot_watermark_;
      cold_watermark_ = original_cold_watermark_;
    } else {
      if (hotter) {
        hot_watermark_ = std::min(static_cast<uint8_t>(hot_watermark_ + 1), 
                                  hot_watermark_upper_bound_);
        cold_watermark_ = original_cold_watermark_;
      } else {
        hot_watermark_ = original_hot_watermark_;
        cold_watermark_ = std::min(static_cast<uint8_t>(cold_watermark_ + 1),
                                  cold_watermark_upper_bound_);         
      }
    }
  }

  inline void adjust_demotion_depth_threshold(bool is_shrink) {
    // the root node of the entire tree should always stay in local memory
    if (is_shrink) {
      demotion_depth_threshold_ = 
        std::max(static_cast<uint8_t>(demotion_depth_threshold_ - 1),
                  default_demotion_depth_threshold_);
    } else {
      demotion_depth_threshold_ = 
        demotion_depth_threshold_ == LEAF_DEPTH ? 
        LEAF_DEPTH : demotion_depth_threshold_ + 1;
    }
  }

private:
  uint8_t local_allocation_layer_threshold_;
  uint8_t demotion_depth_threshold_;
  uint8_t default_demotion_depth_threshold_;
  double upper_watermark_{default_upper_watermark};
  double lower_watermark_{default_lower_watermark};
  uint8_t hot_watermark_{default_hot_watermark};
  uint8_t original_hot_watermark_{default_hot_watermark};
  uint8_t hot_watermark_upper_bound_{default_hot_watermark + 5};
  uint8_t cold_watermark_{default_cold_watermark};
  uint8_t original_cold_watermark_{default_cold_watermark};
  uint8_t cold_watermark_upper_bound_{default_cold_watermark + 10};
};


class sidle_strategy {
public: 
  sidle_threshold threshold_manager_;

  explicit sidle_strategy(uint64_t max_local_memory_usage = default_max_local_memory_usage, int cxl_percentage = 80,
                          bool is_art = true):
      max_local_memory_usage_(max_local_memory_usage * 1024 * 1024) {
#ifdef POLICY_DEBUG
    printf("[DEBUG] [sidle_strategy] max_local_memory_usage: %lu\n", 
      max_local_memory_usage_ / (1024 * 1024));
#endif
    uint64_t node_count = is_art ? 
      max_local_memory_usage_ / 
            ((sizeof(art::art_node4) + sizeof(art::art_node256)) / 2)
      : max_local_memory_usage_ / 
            (Masstree::default_query_table_params::leaf_size + Masstree::default_query_table_params::internode_size) / 2;
    threshold_manager_ = sidle_threshold(node_count, cxl_percentage, is_art);
  } 

  node_mem_type decide_new_node_position(node_mem_type parent_type, 
                                        uint8_t cur_depth) {
    if (parent_type == node_mem_type::remote) {
        return node_mem_type::remote;
    }
#ifdef POLICY_DEBUG
    // static uint64_t record_cnt = 0;
    // if (record_cnt % 10 == 0) {
    //   printf("[DEBUG] [decide_new_node_position] cur_depth: %d\n", cur_depth);
    // }
    // ++record_cnt;
#endif
    return threshold_manager_.get_allocation_position(cur_depth);
  }

  inline sidle_threshold* get_threshold_manager() { return &threshold_manager_; }

  void update_local_memory_usage(node_mem_type type, uint8_t cur_depth,
                                 int64_t node_size,
                                 bool is_migration = false,
                                 bool is_leaf = false) {
#ifdef POLICY_DEBUG
    static uint64_t record_cnt = 0;
#endif
    if (type == node_mem_type::local) {
      cur_local_memory_usage_ += node_size;
    } else if (type == node_mem_type::remote && is_migration) {
      cur_local_memory_usage_ -= node_size;
    }
    if (is_migration && is_leaf && cur_depth > 0) {
      threshold_manager_.correct_threshold_with_leaf_depth(cur_depth);
    }

    SIDLE_CHECK(cur_local_memory_usage_ >= 0, 
        "[update_local_memory_usage] cur_local_memory_usage < 0");
    double memory_usage_ratio = static_cast<double>(cur_local_memory_usage_) / 
                                max_local_memory_usage_;
#ifdef POLICY_DEBUG
    if (record_cnt % 10 == 0) {
      printf("[DEBUG] local memory usage: %ld, max usage: %ld, ratio: %f\n", 
            cur_local_memory_usage_, max_local_memory_usage_, memory_usage_ratio);
    }
    ++record_cnt;
#endif
    mem_usage_status status = threshold_manager_.check_memory_usage(memory_usage_ratio);
    if (unlikely(art_worker_base::need_adjust_threshold_.load(std::memory_order_relaxed))
          && status == mem_usage_status::sufficient) {
      art_worker_base::can_demote_.store(false, std::memory_order_relaxed);
    } else if (status == mem_usage_status::tight) {
      std::lock_guard<std::mutex> lock(art_worker_base::adjuster_wakeup_mtx_);
      art_worker_base::need_adjust_threshold_.store(true, std::memory_order_relaxed);
      art_worker_base::adjuster_wakeup_cv_.notify_one();
    } else if (unlikely(!art_worker_base::is_running_.load(std::memory_order_relaxed)
        && status == mem_usage_status::normal && !slow_down_in_load)) {
      slow_down_in_load = true;
      threshold_manager_.adjust_local_allocation_threshold(true);
    }
  }

  inline mem_usage_status check_memory_usage() {
    double memory_usage_ratio = static_cast<double>(cur_local_memory_usage_) / 
                                max_local_memory_usage_;
    return threshold_manager_.check_memory_usage(memory_usage_ratio);
  }
 
private:
  uint64_t max_local_memory_usage_{0};
  int64_t cur_local_memory_usage_{0};
  bool slow_down_in_load{false};
};

}   // namespace sidle
#endif
