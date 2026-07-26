#ifndef DSIDLE_ENABLE_ARCHIVED_UPSTREAM_BENCHMARK_SOURCE
#error "src/helper.* is archived upstream benchmark support, not a D-SIDLE API"
#else

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#include "query_masstree.hh"

#if !defined(HELPER_H)
#define HELPER_H

#define RETRAIN_THREAD 1

#define COUT_THIS(this) std::cout << this << std::endl;
#define COUT_VAR(this) std::cout << #this << ": " << this << std::endl;
#define COUT_N_EXIT(msg) \
  COUT_THIS(msg);        \
  COUT_POS();            \
  abort();
#define COUT_POS() COUT_THIS("at " << __FILE__ << ":" << __LINE__)
#define INVARIANT2(cond, msg)      \
  if (!(cond)) {                   \
    COUT_THIS(#cond << " failed"); \
    COUT_THIS(msg);                \
    COUT_POS();                    \
    abort();                       \
  }
#define INVARIANT(cond)            \
  if (!(cond)) {                   \
    COUT_THIS(#cond << " failed"); \
    COUT_POS();                    \
    abort();                       \
  }

#if defined(NDEBUGGING)
#define DEBUG_THIS(this)
#else
#define DEBUG_THIS(this) std::cerr << this << std::endl
#endif

#define UNUSED(var) ((void)var)

#define CPU_CYCLE_PER_NANOSEC 2.9
#define CPU_CYCLE_PER_SEC 2900000000

#define CACHELINE_SIZE (1 << 6)

#define PACKED __attribute__((packed))
#define ALWAYS_INLINE __attribute__((always_inline))

/******************************************************************************
 * Decls
 *****************************************************************************/

////////////////////////
// Arithmetic helpers //
////////////////////////
template <class Val_T>
inline void min_max(const std::vector<Val_T> &vals, Val_T &max, Val_T &min);
template <class Val_T>
inline void max(const std::vector<Val_T> &vals, Val_T &max);
template <class Val_T>
inline void max_range(const typename std::vector<Val_T>::const_iterator &begin,
                      const typename std::vector<Val_T>::const_iterator &end,
                      Val_T &max);
template <class Val_T>
inline void mean(const std::vector<Val_T> &vals, double &mean);

////////////////////
// Timing helpers //
////////////////////
static inline uint64_t util_rdtsc(void);
void timeit();

////////////////////
// Matrix helpers //
////////////////////
void print_matrix(double *data, int row_n, int col_n);
void print_matrix(const uint8_t *data, const size_t row_n, const size_t col_n);

int bind_to_physical_cores(int t_id);
int bind_to_physical_cores_reverse(int t_id);

/******************************************************************************
 * Template/inline impls
 *****************************************************************************/

template <class Val_T>
inline void min_max(const std::vector<Val_T> &vals, Val_T &max, Val_T &min) {
  assert(vals.size() != 0);

  max = vals[0];
  min = vals[0];
  for (const Val_T &val : vals) {
    if (val > max) max = val;
    if (val < min) min = val;
  }
}

template <class Val_T>
inline void max(const std::vector<Val_T> &vals, Val_T &max) {
  assert(vals.size() != 0);

  max = vals[0];
  for (const Val_T &val : vals) {
    if (val > max) max = val;
  }
}

template <class Val_T>
inline void max_range(const typename std::vector<Val_T>::const_iterator &begin,
                      const typename std::vector<Val_T>::const_iterator &end,
                      Val_T &max) {
  assert(end - begin != 0);

  max = *begin;
  for (typename std::vector<Val_T>::const_iterator iter = begin; iter != end;
       iter++) {
    if (max < *iter) max = *iter;
  }
}

template <class Val_T>
inline void mean(const std::vector<Val_T> &vals, double &mean) {
  double sum = 0;
  for (const Val_T &val : vals) {
    sum += val;
  }

  mean = sum / vals.size();
}

static inline uint64_t util_rdtsc(void) {
  uint32_t hi, lo;
  __asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

static inline uint64_t util_rdtsc_start(void) {
  uint32_t hi, lo;
  __asm volatile(
      "CPUID\n\t"
      "RDTSC\n\t"
      "mov %%edx, %0\n\t"
      "mov %%eax, %1\n\t"
      : "=r"(hi), "=r"(lo)::"%rax", "%rbx", "%rcx", "%rdx");
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

static inline uint64_t util_rdtsc_end(void) {
  uint32_t hi, lo;
  __asm volatile(
      "RDTSCP\n\t"
      "mov %%edx, %0\n\t"
      "mov %%eax, %1\n\t"
      "CPUID\n\t"
      : "=r"(hi), "=r"(lo)::"%rax", "%rbx", "%rcx", "%rdx");
  return ((uint64_t)lo) | (((uint64_t)hi) << 32);
}

inline uint64_t rdtscp(uint32_t &aux) {
  uint64_t rax, rdx;
  asm volatile("rdtscp\n" : "=a"(rax), "=d"(rdx), "=c"(aux) : :);
  return (rdx << 32) + rax;
}

inline void rdtscp() {
  uint64_t rax, rdx;
  asm volatile("rdtscp\n" : "=a"(rax), "=d"(rdx), "=c"(rax) : :);
}

template <class key_t>
struct SortedArrayKeyCompare {
  bool operator()(const uint8_t *left, const uint8_t *right) const {
    return *((key_t *)left) < *((key_t *)right);
  }
};

template <size_t key_size, size_t key_seg_size>
struct StrCompare {
  bool operator()(const std::string &left, const std::string &right) {
    assert(key_seg_size == 1);
    return left < right;
  }
};

template <class key_t, class val_t, class row_t>
class RangeScanCallback {
 public:
  RangeScanCallback(const key_t &begin, const key_t &end,
                    std::vector<std::pair<key_t, val_t>> &result)
      : begin(begin), end(end), result(result) {
    result.clear();
  }

  bool operator()(Str &key, row_t *value, threadinfo &ti, query<row_t> &q) {
    key_t &masstree_key = *(key_t *)(key.data());
    int cmp_res =
        memcmp((uint8_t *)&masstree_key, (uint8_t *)&end, sizeof(key_t));
    if (cmp_res >= 0) {
      return false;
    }

    // extract value
    const row_t *snapshot = q.helper_.snapshot(value, q.f_, ti);
    if ((q.f_.empty() && snapshot->ncol() == 1) || q.f_.size() == 1) {
      val_t &masstree_val =
          *(val_t *)(snapshot->col(q.f_.empty() ? 0 : q.f_[0]).data());
      result.emplace_back(masstree_key.to_normal_key(), masstree_val);
    } else {
      COUT_N_EXIT("TCZ didn't implement this, so exiting!");
    }

    return true;
  }

 private:
  key_t begin, end;
  std::vector<std::pair<key_t, val_t>> &result;
};

template <class val_t>
class RemoveVal {
 public:
  static const val_t &get() {
    static constexpr uint8_t raw[sizeof(val_t)] = {};
    return *(val_t *)raw;
  }
};

inline const bool little_endian() {
  int num = 1;
  return *(char *)&num == 1;
}

#endif  // HELPER_H

#endif  // DSIDLE_ENABLE_ARCHIVED_UPSTREAM_BENCHMARK_SOURCE
