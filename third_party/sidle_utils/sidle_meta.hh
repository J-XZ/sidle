#ifndef SIDLE_COMMON_HH
#define SIDLE_COMMON_HH

#include "concurrentqueue.hh"
#include "dsidle/node_control.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

#ifdef NDEBUG
    #define SIDLE_CHECK(condition, msg) ((void)0)
#else
    #define SIDLE_CHECK(condition, msg) do { \
        if (!(condition)) { \
            throw std::runtime_error(msg); \
        } \
    } while (0)
#endif

#define SIDLE_RECORD(ENABLED, fmt, ...) do { \
    if (ENABLED) { \
        printf(fmt, ##__VA_ARGS__); \
    } \
} while (0)


namespace sidle {
enum class worker_type {
    migration_trigger, promotion_executor, demotion_executor, cooler, threshold_adjuster
};

enum class node_mem_type : uint8_t {
    remote = 0, local, unknown, debug
};

enum class task_type : uint8_t {
    promotion, demotion
};

enum class mem_usage_status : uint8_t {
    normal = 0,
    tight,
    sufficient,
};

enum class migration_state : uint8_t {
    proceed = 0,
    delay,
    stop
};

class node_mem_type_helper {
public:
    static std::string to_string(const node_mem_type &type) {
        switch (type) {
            case node_mem_type::remote:
                return "remote";
            case node_mem_type::local:
                return "local";
            case node_mem_type::unknown:
                return "unknown";
            case node_mem_type::debug:
                return "debug";
        default:
                return "unknown";
        }
        return "unknown";
    }
};


/// @brief singleton class, contains a concurrent queue for storing nodes' metadata for migration (promotion and demotion)
class migration_queue {
private:
    using node_pair = std::pair<uint64_t, uint64_t>;
    // the node's address for migration
    // use multi-get/put or single-get/put exclusively
    std::shared_ptr<moodycamel::ConcurrentQueue<uint64_t>> promotion_queue_;
    std::shared_ptr<moodycamel::ConcurrentQueue<uint64_t>> demotion_queue_;
    std::shared_ptr<moodycamel::ConcurrentQueue<node_pair>> promotion_pair_queue_;
    std::shared_ptr<moodycamel::ConcurrentQueue<node_pair>> demotion_pair_queue_;
    std::atomic_int32_t promotion_queue_length_;
    std::atomic_int32_t demotion_queue_length_;

public:
    migration_queue(): promotion_queue_(std::make_shared<moodycamel::ConcurrentQueue<uint64_t>>()), 
        demotion_queue_(std::make_shared<moodycamel::ConcurrentQueue<uint64_t>>()),
        promotion_pair_queue_(std::make_shared<moodycamel::ConcurrentQueue<node_pair>>()),
        demotion_pair_queue_(std::make_shared<moodycamel::ConcurrentQueue<node_pair>>()), 
        promotion_queue_length_(0), 
        demotion_queue_length_(0) { }
    migration_queue(migration_queue const&) = delete;
    migration_queue& operator=(migration_queue const&) = delete;

    uint64_t get_task(task_type type) {
        uint64_t result = 0;
        if (type == task_type::promotion) {
            promotion_queue_->try_dequeue(result);
            if (result != 0) {
                promotion_queue_length_.fetch_add(-1, std::memory_order_relaxed);
            }
        } else {
            demotion_queue_->try_dequeue(result);
            if (result != 0) {
                demotion_queue_length_.fetch_add(-1, std::memory_order_relaxed);
            }
        }
        return result;
    }

    node_pair get_multi_task(task_type type) {
        node_pair result = std::make_pair(0, 0);
        if (type == task_type::promotion) {
            promotion_pair_queue_->try_dequeue(result);
            if (result.first != 0) {
                promotion_queue_length_.fetch_add(-1, std::memory_order_relaxed);
            } 
        } else {
            demotion_pair_queue_->try_dequeue(result);
            if (result.first != 0) {
                demotion_queue_length_.fetch_add(-1, std::memory_order_relaxed);
            }
        }
        return result;
    }

    void add_task(task_type type, uint64_t data) {
        if (type == task_type::promotion) {
            promotion_queue_->enqueue(data);
            promotion_queue_length_.fetch_add(1, std::memory_order_relaxed);
        } else {
            demotion_queue_->enqueue(data);
            demotion_queue_length_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void add_multi_task(task_type type, const node_pair& data) {
       if (type == task_type::promotion) {
            promotion_pair_queue_->enqueue(data);
            promotion_queue_length_.fetch_add(1, std::memory_order_relaxed);
       } else {
            demotion_pair_queue_->enqueue(data);
            demotion_queue_length_.fetch_add(1, std::memory_order_relaxed);
       }
    }   

    std::size_t get_promotion_queue_length() const {
        return std::max(promotion_queue_length_.load(std::memory_order_relaxed), 0);
    }

    std::size_t get_demotion_queue_length() const {
        return std::max(demotion_queue_length_.load(std::memory_order_relaxed), 0);
    }
};

// D-SIDLE's policy queue deliberately has no virtual address payload.  It is
// process-local like the original MoodyCamel queue, but a dequeued candidate
// is validated against NodeControl.generation before any replica action.
class replica_queue {
 public:
  replica_queue()
      : promotion_(std::make_shared<moodycamel::ConcurrentQueue<dsidle::QueuedNodeRef>>()),
        demotion_(std::make_shared<moodycamel::ConcurrentQueue<dsidle::QueuedNodeRef>>()) {}

  void add(task_type type, dsidle::QueuedNodeRef candidate) {
    if (!candidate) return;
    auto& queue = type == task_type::promotion ? promotion_ : demotion_;
    queue->enqueue(candidate);
    (type == task_type::promotion ? promotion_length_ : demotion_length_)
        .fetch_add(1, std::memory_order_relaxed);
  }

  bool get(task_type type, dsidle::QueuedNodeRef& candidate) {
    auto& queue = type == task_type::promotion ? promotion_ : demotion_;
    if (!queue->try_dequeue(candidate)) return false;
    (type == task_type::promotion ? promotion_length_ : demotion_length_)
        .fetch_sub(1, std::memory_order_relaxed);
    return true;
  }

  std::size_t length(task_type type) const {
    const auto value = (type == task_type::promotion ? promotion_length_ : demotion_length_)
        .load(std::memory_order_relaxed);
    return value > 0 ? static_cast<std::size_t>(value) : 0;
  }

 private:
  std::shared_ptr<moodycamel::ConcurrentQueue<dsidle::QueuedNodeRef>> promotion_;
  std::shared_ptr<moodycamel::ConcurrentQueue<dsidle::QueuedNodeRef>> demotion_;
  std::atomic_int32_t promotion_length_{0};
  std::atomic_int32_t demotion_length_{0};
};

} // namespace sidle

#endif
