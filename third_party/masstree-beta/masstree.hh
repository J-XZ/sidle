/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2014 President and Fellows of Harvard College
 * Copyright (c) 2012-2014 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef MASSTREE_HH
#define MASSTREE_HH

#include <limits>

#include "compiler.hh"
#include "ksearch.hh"
#include "str.hh"
#include "dsidle/shared_pool.h"

// #define CAL_NODE_HOTNESS

namespace Masstree {
using lcdf::Str;
using lcdf::String;

class key_unparse_printable_string;
template <typename T> class value_print;

template <int LW = 15, int IW = LW> struct nodeparams {
  static constexpr int leaf_width = LW;
  static constexpr int internode_width = IW;
  static constexpr bool concurrent = true;
  static constexpr bool prefetch = true;
  static constexpr int bound_method = bound_method_binary;
  static constexpr int debug_level = 0;
  static constexpr int leaf_size = 320;
  static constexpr int internode_size = 272;
  typedef uint64_t ikey_type;
  typedef uint32_t nodeversion_value_type;
  static constexpr bool need_phantom_epoch = true;
  typedef uint64_t phantom_epoch_type;
  static constexpr ssize_t print_max_indent_depth = 12;
  typedef key_unparse_printable_string key_unparse_type;
};

struct migrationparams {
  static constexpr int leaf_width = 16;
  static constexpr uint64_t local_threshold = 0;
  static constexpr uint64_t migration_threshold =
      std::numeric_limits<uint64_t>::max();
#ifdef REAL_WORLD
  static constexpr uint16_t hot_threshold = 64;
  static constexpr uint16_t cold_threshold = 2;
#else
  static constexpr uint16_t hot_threshold = 128;
  static constexpr uint16_t cold_threshold = 10;
#endif
  static constexpr uint8_t max_local_depth = 5;
  /// @brief the waiting threshold is the lower bound for triggering migration
  /// execution
  static constexpr uint64_t waiting_threshold = 10;
  /// @brief percentage for dynamically deciding hot threshold
  static constexpr uint64_t hot_watermark = 5;
  /// @brief limit the variation range of hot watermark
  static constexpr uint64_t hot_watermark_upper_bound = 10;
  /// @brief percentage for dynamically deciding cold threshold
  static constexpr uint64_t cold_watermark = 80;
  /// @brief limit the variation range of cold watermark
  static constexpr uint64_t cold_watermark_upper_bound = 90;
  /// @brief local memory usage upper bound
  static constexpr double upper_watermark = 0.95;
  /// @brief local memory usage lower bound
  static constexpr double lower_watermark = 0.85;
  /// @brief the max number of local nodes
#ifdef SHORT_RANGE
  // 130MB
  // static constexpr uint64_t max_local_node_count = 440000;
  static constexpr uint64_t max_local_memory_usage = 130;
#elif defined(READ_LATEST)
  // 250MB
  // static constexpr uint64_t max_local_node_count = 840000;
  static constexpr uint64_t max_local_memory_usage = 250;
#elif defined(SKEW)
  // 66MB
  // static constexpr uint64_t max_local_node_count = 220000;
  static constexpr uint64_t max_local_memory_usage = 66;
#elif defined(SKEW_DYNAMIC)
  // 122MB
  // static constexpr uint64_t max_local_node_count = 410000;
  static constexpr uint64_t max_local_memory_usage = 120;
#else
  // 86MB
  // static constexpr uint64_t max_local_node_count = 290000;
  static constexpr uint64_t max_local_memory_usage = 86;
#endif
};

template <int LW, int IW> constexpr int nodeparams<LW, IW>::leaf_width;
template <int LW, int IW> constexpr int nodeparams<LW, IW>::internode_width;
template <int LW, int IW> constexpr int nodeparams<LW, IW>::debug_level;

template <typename P> class node_base;
template <typename P> class leaf;
template <typename P> class internode;
template <typename P> class leafvalue;
template <typename P> class key;
template <typename P> class basic_table;
template <typename P> class unlocked_tcursor;
template <typename P> class tcursor;

template <typename P> class basic_table {
public:
  typedef P parameters_type;
  typedef node_base<P> node_type;
  typedef leaf<P> leaf_type;
  typedef typename P::value_type value_type;
  typedef typename P::threadinfo_type threadinfo;
  typedef unlocked_tcursor<P> unlocked_cursor_type;
  typedef tcursor<P> cursor_type;

  inline basic_table();

  void initialize(threadinfo &ti, const int cxl_percentage);
  void attach();
  void destroy(threadinfo &ti);

  inline node_type *root() const;
  inline node_type *fix_root();
  inline dsidle::NodeRef stable_root_ref() const;

  bool get(Str key, value_type &value, threadinfo &ti) const;

  template <typename F>
  int scan(Str firstkey, bool matchfirst, F &scanner, threadinfo &ti) const;
  template <typename F>
  int rscan(Str firstkey, bool matchfirst, F &scanner, threadinfo &ti) const;

  inline void print(FILE *f = 0) const;

private:
  // dsidle: the persistent root identity lives in HWCC; root() resolves a
  // stack-local canonical pointer for the original traversal code.
  dsidle::NodeRef root_ref_;

  template <typename H, typename F>
  int scan(H helper, Str firstkey, bool matchfirst, F &scanner,
           threadinfo &ti) const;

  friend class unlocked_tcursor<P>;
  friend class tcursor<P>;
};

} // namespace Masstree
#endif
