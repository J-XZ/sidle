#ifndef MASSTREE_INTERNAL_REPLICA_HH
#define MASSTREE_INTERNAL_REPLICA_HH

#include "masstree_struct.hh"
#include "dsidle/replica_directory.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace Masstree {

// Process-local internode representation. Child edges remain NodeRef, so a
// hit accelerates only this node and never makes a local subtree authoritative.
template <typename P>
class internode_replica {
 public:
  using node_type = internode<P>;
  using key_type = typename node_type::key_type;
  using ikey_type = typename node_type::ikey_type;

  struct header { std::uint32_t nkeys{}; std::uint32_t bytes{}; };

  static void* Create(const node_type& source) {
    const auto nkeys = static_cast<std::uint32_t>(source.size());
    const auto bytes = sizeof(header) + static_cast<std::size_t>(nkeys) * sizeof(ikey_type) +
                       static_cast<std::size_t>(nkeys + 1) * sizeof(dsidle::NodeRef);
    auto* memory = static_cast<std::byte*>(std::malloc(bytes));
    if (!memory) throw std::bad_alloc();
    new (memory) header{nkeys, static_cast<std::uint32_t>(bytes)};
    auto* keys = reinterpret_cast<ikey_type*>(memory + sizeof(header));
    auto* children = reinterpret_cast<dsidle::NodeRef*>(keys + nkeys);
    for (std::uint32_t index = 0; index < nkeys; ++index) keys[index] = source.ikey(index);
    for (std::uint32_t index = 0; index <= nkeys; ++index) children[index] = source.child_[index].ref();
    return memory;
  }

  static dsidle::NodeRef LookupChild(const void* replica, const key_type& key) {
    const auto* memory = static_cast<const std::byte*>(replica);
    const auto* data = static_cast<const header*>(replica);
    const auto* keys = reinterpret_cast<const ikey_type*>(memory + sizeof(header));
    const auto* children = reinterpret_cast<const dsidle::NodeRef*>(keys + data->nkeys);
    std::uint32_t position = 0;
    while (position < data->nkeys && ::compare(key.ikey(), keys[position]) >= 0) ++position;
    return children[position];
  }

  static bool Promote(const node_type& source, typename node_type::nodeversion_type version,
                      dsidle::ReplicaDirectory& directory, bool budgeted = true) {
    const auto ref = source.control_ref();
    const auto generation = ref.get(dsidle::SharedPoolBase())->generation;
    void* buffer = Create(source);
    if (source.has_changed(version)) { std::free(buffer); return false; }
    const auto bytes = static_cast<const header*>(buffer)->bytes;
    void* old = nullptr;
    const dsidle::ReplicaSnapshot snapshot{
        buffer, generation, version.version_value(), bytes,
        dsidle::ReplicaKind::kInternal};
    if (budgeted) {
      if (!directory.TryPublish(ref, snapshot, &old)) {
        std::free(buffer);
        return false;
      }
    } else {
      old = directory.Publish(ref, snapshot);
    }
    std::free(old);
    return true;
  }
};

}  // namespace Masstree

#endif
