#ifndef SIDLE_FRONTEND_HH
#define SIDLE_FRONTEND_HH

#include <cstddef>
#include <type_traits>
#include <sys/types.h>
#include "dsidle/shared_pool.h"
#include "sidle_policy.hh"

namespace sidle {

extern sidle_strategy strategy_manager;

/// @brief allocate node according to single_boundary allocation or migration
/// @tparam T is the type of target node, P is the type of parent node
/// @param old_size a sepecial field for node expansion
/// @return 
template <typename T, typename P>
T* sidle_alloc(size_t size, P* parent, sidle::node_mem_type target_type, 
                      bool is_migration, bool is_leaf, size_t old_size = 0) {
  uint8_t cur_depth = parent == nullptr ? 1 : parent->sidle_meta.depth + 1;
  // dsidle: canonical nodes are always SWCC. SIDLE's selection logic remains
  // for later replica promotion/demotion, never for canonical node placement.
  (void) target_type;
  (void) is_migration;
  (void) old_size;
  const auto new_node_type = sidle::node_mem_type::remote;
  T* an = reinterpret_cast<T*>(dsidle::AllocateCurrentSwcc(size).get(dsidle::SharedPoolBase()));
  if constexpr (std::is_same_v<decltype(an->sidle_meta), sidle::node_metadata>) {
    an->sidle_meta = sidle::node_metadata(new_node_type, cur_depth);
  } else {
    an->sidle_meta = sidle::leaf_metadata(new_node_type, cur_depth, 0);
  }

  (void) is_leaf;
  return an;
}

/// @brief free the node allocated by sidle_alloc
/// @tparam T is the type of the node
/// @param size is the size of the node
template <typename T>
void sidle_free(T* an, size_t size) {
  dsidle::FreeCurrentSwcc(dsidle::SwccOffset<std::byte>(
      reinterpret_cast<std::byte*>(an) - static_cast<std::byte*>(dsidle::SharedPoolBase())), size);
}

/// @brief update the access count of the leaf node
/// @tparam T is the leaf node type
template <typename T>
void update_access(T* leaf) {
  SIDLE_CHECK(leaf != nullptr, 
      "[DEBUG] [update_access] the leaf node shouldn't be nullptr");
  leaf->sidle_meta.access_time += 1;
}
}   // namespace sidle

#endif /* SIDLE_FRONTEND_HH */
