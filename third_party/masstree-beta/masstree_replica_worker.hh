#ifndef MASSTREE_REPLICA_WORKER_HH
#define MASSTREE_REPLICA_WORKER_HH

#include "masstree_internal_replica.hh"
#include "masstree_replica.hh"
#include "sidle_meta.hh"

#include <cstdlib>

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

}  // namespace Masstree

#endif
