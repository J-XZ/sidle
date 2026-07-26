#ifndef MASSTREE_ROOT_REPLICA_HH
#define MASSTREE_ROOT_REPLICA_HH

#include "masstree_internal_replica.hh"
#include "masstree_replica.hh"

#include <cstdlib>

namespace Masstree {

// Per-VM root pin.  The pinned object follows RootControl rather than the
// initial root allocation, so root splits/layer replacements cannot strand a
// stale local root buffer.
template <typename P>
class root_replica_pin {
 public:
  root_replica_pin(dsidle::SharedPool& pool, dsidle::ReplicaDirectory& directory)
      : pool_(pool), directory_(directory) {}
  ~root_replica_pin() {
    // A short runner can stop before Refresh() wins a promotion race, leaving
    // this per-VM pin empty. ReplicaDirectory only accepts real NodeRefs.
    if (ref_) std::free(directory_.Invalidate(ref_));
  }

  bool Refresh(typename P::threadinfo_type& ti) {
    typename P::threadinfo_type::rcu_scope rcu(ti);
    const auto root = dsidle::RootControlAccessor(pool_.root_control()).stable();
    if (!root.ref) return false;
    auto* node = dsidle::ResolveCanonicalNode<node_base<P>>(root.ref);
    const auto version = node->stable();
    const auto generation = dsidle::LoadNodeGeneration(root.ref);
    auto existing = directory_.Acquire(root.ref, generation, version.version_value());
    if (root.version == root_version_ && root.ref == ref_ && existing) return true;
    existing = {};
    bool published = node->isleaf()
        ? leaf_replica<P>::Promote(*static_cast<leaf<P>*>(node), version, directory_, false)
        : internode_replica<P>::Promote(*static_cast<internode<P>*>(node), version, directory_, false);
    if (!published) return false;
    const auto after =
        dsidle::RootControlAccessor(pool_.root_control()).stable();
    if (after.ref != root.ref ||
        after.generation != root.generation ||
        after.version != root.version) {
      std::free(directory_.Invalidate(root.ref));
      return false;
    }
    if (ref_ && ref_ != root.ref) std::free(directory_.Invalidate(ref_));
    ref_ = root.ref;
    root_version_ = root.version;
    return true;
  }

  dsidle::NodeRef ref() const { return ref_; }
  std::uint64_t root_version() const { return root_version_; }

 private:
  dsidle::SharedPool& pool_;
  dsidle::ReplicaDirectory& directory_;
  dsidle::NodeRef ref_{};
  std::uint64_t root_version_{};
};

}  // namespace Masstree

#endif
