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
#ifndef MASSTREE_GET_HH
#define MASSTREE_GET_HH
#include "masstree_tcursor.hh"
#include "masstree_key.hh"
#include "masstree_replica.hh"
#include "masstree_internal_replica.hh"
namespace Masstree {

template <typename P>
bool unlocked_tcursor<P>::find_unlocked(threadinfo& ti)
{
    int match;
    key_indexed_position kx;
    node_base<P>* root = const_cast<node_base<P>*>(root_);
    dsidle::NodeRef root_ref = root_ref_;

 retry:
    replica_handle_ = {};
    replica_value_ = nullptr;
    if (replica_enabled_) {
        if (auto* directory = dsidle::CurrentReplicaDirectoryOrNull()) {
            dsidle::NodeRef replica_ref = root_ref;
            while (replica_ref) {
                dsidle::StableNodeIdentity identity;
                if (!dsidle::TryLoadStableNodeIdentity(replica_ref, &identity)) {
                    root = dsidle::ResolveCanonicalNode<node_base<P>>(replica_ref);
                    break;
                }
                auto handle =
                    directory->Acquire(replica_ref, identity.generation,
                                       identity.version);
                if (!handle) {
                    root = dsidle::ResolveCanonicalNode<node_base<P>>(replica_ref);
                    break;
                }
                const auto snapshot = handle.snapshot();
                if (snapshot.kind == dsidle::ReplicaKind::kInternal) {
                    const auto child_ref =
                        internode_replica<P>::LookupChild(snapshot.local_ptr,
                                                         ka_);
                    dsidle::StableNodeIdentity after;
                    if (!dsidle::TryLoadStableNodeIdentity(replica_ref, &after) ||
                        after.generation != identity.generation ||
                        after.version != identity.version) {
                        root =
                            dsidle::ResolveCanonicalNode<node_base<P>>(replica_ref);
                        break;
                    }
                    directory->RecordInternalHit();
                    if (!child_ref) {
                        root = dsidle::ResolveCanonicalNode<node_base<P>>(
                            replica_ref);
                        break;
                    }
                    replica_ref = child_ref;
                    continue;
                }

                const typename leaf_replica<P>::value_type* local_value =
                    nullptr;
                dsidle::NodeRef layer_ref;
                const auto cached =
                    leaf_replica<P>::Lookup(snapshot.local_ptr, ka_,
                                            local_value, layer_ref);
                dsidle::StableNodeIdentity after;
                if (!dsidle::TryLoadStableNodeIdentity(replica_ref, &after) ||
                    after.generation != identity.generation ||
                    after.version != identity.version) {
                    root =
                        dsidle::ResolveCanonicalNode<node_base<P>>(replica_ref);
                    break;
                }
                if (cached == leaf_replica<P>::result::kValue) {
                    // A local SIDLE leaf hit is still an access to that leaf.
                    // Keep the per-VM counter semantics without touching its
                    // canonical SWCC metadata.
                    directory->RecordAccess(replica_ref);
                    // `n_` preserves the cursor's identity API only. The value
                    // comes from the protected local snapshot, so do not
                    // establish SWCC visibility for the canonical leaf.
                    n_ = dsidle::CanonicalNodeAddressFromStableIdentity<
                        leaf<P>>(replica_ref);
                    v_ = nodeversion_type::make_snapshot(
                        static_cast<nodeversion_value_type>(identity.version));
                    perm_ = permuter_type(permuter_type::make_sorted(
                        static_cast<const typename leaf_replica<P>::header*>(
                            snapshot.local_ptr)->count));
                    replica_value_ = const_cast<value_type>(local_value);
                    replica_handle_ = std::move(handle);
                    return true;
                }
                if (cached == leaf_replica<P>::result::kLayer) {
                    directory->RecordAccess(replica_ref);
                    ka_.shift_by(int(sizeof(typename P::ikey_type)));
                    root_ref = layer_ref;
                    replica_ref = layer_ref;
                    continue;
                }
                root =
                    dsidle::ResolveCanonicalNode<node_base<P>>(replica_ref);
                break;
            }
        }
    }

    if (!root)
        root = dsidle::ResolveCanonicalNode<node_base<P>>(root_ref);
    n_ = root->reach_leaf(ka_, v_, ti);

 forward:
    if (v_.deleted())
        goto retry;

    if (replica_enabled_) if (auto* directory = dsidle::CurrentReplicaDirectoryOrNull()) {
        const auto ref = n_->control_ref();
        const auto generation = dsidle::LoadNodeGeneration(ref);
        auto handle = directory->Acquire(ref, generation, v_.version_value());
        if (handle) {
            const typename Masstree::leaf_replica<P>::value_type* local_value = nullptr;
            dsidle::NodeRef layer_ref;
            const auto cached = Masstree::leaf_replica<P>::Lookup(
                handle.snapshot().local_ptr, ka_, local_value, layer_ref);
            if (n_->has_changed(v_)) {
                n_ = n_->advance_to_key(ka_, v_, ti);
                goto forward;
            }
            if (cached == Masstree::leaf_replica<P>::result::kValue) {
                replica_value_ = const_cast<value_type>(local_value);
                replica_handle_ = std::move(handle);
                return true;
            }
            if (cached == Masstree::leaf_replica<P>::result::kLayer) {
                ka_.shift_by(int(sizeof(typename P::ikey_type)));
                root_ref = layer_ref;
                root = dsidle::ResolveCanonicalNode<node_base<P>>(root_ref);
                goto retry;
            }
            // A replica is only a local read cache.  A miss in its compact
            // snapshot is not authoritative, so preserve Masstree's canonical
            // lookup as the correctness fallback.
        }
    }

    n_->prefetch();
    perm_ = n_->permutation();
    kx = leaf<P>::bound_type::lower(ka_, *this);
    if (kx.p >= 0) {
        lv_ = n_->lv_[kx.p];
        lv_.prefetch(n_->keylenx_[kx.p]);
        match = n_->ksuf_matches(kx.p, ka_);
    } else
        match = 0;
    if (n_->has_changed(v_)) {
        ti.mark(threadcounter(tc_stable_leaf_insert + n_->simple_has_split(v_)));
        n_ = n_->advance_to_key(ka_, v_, ti);
        goto forward;
    }

    if (match < 0) {
        ka_.shift_by(-match);
        root_ref = lv_.layer_ref();
        // Give the next layer's local replica the first chance. A miss will
        // resolve the same NodeRef through the unchanged canonical path.
        root = nullptr;
        goto retry;
    } else
        return match;
}

template <typename P>
inline bool basic_table<P>::get(Str key, value_type &value,
                                threadinfo& ti) const
{
    unlocked_tcursor<P> lp(*this, key);
    // This legacy pointer-returning API cannot retain the directory read
    // handle past return; query APIs copy values while their cursor lives.
    lp.disable_replica();
    bool found = lp.find_unlocked(ti);
    if (found)
        value = lp.value();
    return found;
}

template <typename P>
bool tcursor<P>::find_locked(threadinfo& ti)
{
    node_base<P>* root = const_cast<node_base<P>*>(root_);
    nodeversion_type v;
    permuter_type perm;

 retry:
    n_ = root->reach_leaf(ka_, v, ti);

 forward:
    if (v.deleted())
        goto retry;

    n_->prefetch();
    perm = n_->permutation();
    fence();
    kx_ = leaf<P>::bound_type::lower(ka_, *n_);
    if (kx_.p >= 0) {
        leafvalue<P> lv = n_->lv_[kx_.p];
        lv.prefetch(n_->keylenx_[kx_.p]);
        state_ = n_->ksuf_matches(kx_.p, ka_);
        if (state_ < 0 && !n_->has_changed(v, false) && lv.layer()->is_root()) {
            ka_.shift_by(-state_);
            root = lv.layer();
            goto retry;
        }
    } else
        state_ = 0;

    n_->lock(v, ti.lock_fence(tc_leaf_lock));
    if (n_->has_changed(v, false) || n_->permutation() != perm) {
        ti.mark(threadcounter(tc_stable_leaf_insert + n_->simple_has_split(v)));
        n_->unlock();
        n_ = n_->advance_to_key(ka_, v, ti);
        goto forward;
    } else if (unlikely(state_ < 0)) {
        ka_.shift_by(-state_);
        n_->lv_[kx_.p] = root = n_->lv_[kx_.p].layer()->maybe_parent();
        n_->unlock();
        goto retry;
    } else if (unlikely(n_->deleted_layer())) {
        ka_.unshift_all();
        root = const_cast<node_base<P>*>(root_);
        n_->unlock();
        goto retry;
    }
    return state_;
}

} // namespace Masstree
#endif
