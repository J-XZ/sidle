/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2016 President and Fellows of Harvard College
 * Copyright (c) 2012-2016 Massachusetts Institute of Technology
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
#ifndef MASSTREE_STRUCT_HH
#define MASSTREE_STRUCT_HH
#include "cxl_cpp_allocator.hh"
#include "masstree.hh"
#include "nodeversion.hh"
#include "stringbag.hh"
#include "mtcounters.hh"
#include "timestamp.hh"
#include "sidle_frontend.hh"
#include "dsidle/replica_directory.h"
#include <array>
#ifdef CAL_NODE_HOTNESS
#include <unordered_map>
#include <vector>
#endif

namespace Masstree {

template <typename P> class internode_replica;
template <typename P> class leaf_replica;

// Owns an allocated canonical SWCC body until its paired NodeControl has been
// published. Any exception before Commit returns both resources; an
// ALLOCATING control is never left stranded.
template <typename ThreadInfo>
class canonical_allocation_guard {
  public:
    canonical_allocation_guard(ThreadInfo& ti, void* body, size_t size,
                               memtag tag)
        : ti_(ti), body_(body), size_(size), tag_(tag) {
    }
    canonical_allocation_guard(const canonical_allocation_guard&) = delete;
    canonical_allocation_guard& operator=(
        const canonical_allocation_guard&) = delete;
    ~canonical_allocation_guard() noexcept {
        if (!body_)
            return;
        try {
            if (control_ref_)
                controls_->Cancel(control_ref_);
            ti_.pool_deallocate(body_, size_, tag_);
        } catch (...) {
            // Cleanup failure indicates corrupted allocator/control state;
            // continuing would make the pair mismatch externally visible.
            std::terminate();
        }
    }
    void pair(dsidle::NodeControlSlab& controls, dsidle::NodeRef ref) {
        controls_ = &controls;
        control_ref_ = ref;
    }
    void commit() {
        body_ = nullptr;
        controls_ = nullptr;
        control_ref_ = {};
    }

  private:
    ThreadInfo& ti_;
    void* body_;
    size_t size_;
    memtag tag_;
    dsidle::NodeControlSlab* controls_{};
    dsidle::NodeRef control_ref_{};
};

// dsidle: 8-byte persistent tree edge.  Conversions only resolve a transient
// address at the use site; no process virtual address is stored in SWCC.
template <typename T>
class node_link {
  public:
    constexpr node_link() = default;
    node_link(std::nullptr_t) : ref_() {}
    node_link(T* node) { *this = node; }
    node_link& operator=(T* node) {
        ref_ = node ? node->control_ref() : dsidle::NodeRef();
        return *this;
    }
    node_link& operator=(std::nullptr_t) {
        ref_ = dsidle::NodeRef();
        return *this;
    }
    operator T*() const { return dsidle::ResolveCanonicalNode<T>(ref_); }
    T* operator->() const { return dsidle::ResolveCanonicalNode<T>(ref_); }
    explicit operator bool() const { return static_cast<bool>(ref_); }
    dsidle::NodeRef ref() const { return ref_; }

  private:
    dsidle::NodeRef ref_{};
};
static_assert(sizeof(node_link<void>) == sizeof(std::uint64_t));

template <typename T>
class tagged_node_link {
  public:
    constexpr tagged_node_link() = default;
    tagged_node_link& operator=(T* node) {
        raw_ = node ? node->control_ref().value() : 0;
        return *this;
    }
    operator T*() const { return dsidle::ResolveCanonicalNode<T>(dsidle::NodeRef(raw_ & ~std::uint64_t(1))); }
    explicit operator bool() const { return raw_ != 0; }
    dsidle::NodeRef ref() const { return dsidle::NodeRef(raw_ & ~std::uint64_t(1)); }
    std::uint64_t raw_{};
};
static_assert(sizeof(tagged_node_link<void>) == sizeof(std::uint64_t));

template <typename T>
class swcc_link {
  public:
    constexpr swcc_link() = default;
    swcc_link(std::nullptr_t) : ref_() {}
    swcc_link(T* value) { *this = value; }
    swcc_link& operator=(T* value) {
        ref_ = value ? dsidle::SwccOffset<T>(reinterpret_cast<std::byte*>(value) -
                                             static_cast<std::byte*>(dsidle::SharedPoolBase()))
                     : dsidle::SwccOffset<T>();
        return *this;
    }
    swcc_link& operator=(std::nullptr_t) {
        ref_ = dsidle::SwccOffset<T>();
        return *this;
    }
    operator T*() const { return ref_.get(dsidle::SharedPoolBase()); }
    T* operator->() const { return ref_.get(dsidle::SharedPoolBase()); }
    explicit operator bool() const { return static_cast<bool>(ref_); }
    dsidle::SwccOffset<T> ref() const { return ref_; }

  private:
    dsidle::SwccOffset<T> ref_{};
};
static_assert(sizeof(swcc_link<void>) == sizeof(std::uint64_t));

template <typename P>
struct make_nodeversion {
    typedef nodeversion_parameters<typename P::nodeversion_value_type> parameters_type;
    typedef typename mass::conditional<P::concurrent,
                                       nodeversion<parameters_type>,
                                       singlethreaded_nodeversion<parameters_type> >::type type;
};

template <typename P>
struct make_prefetcher {
    typedef typename mass::conditional<P::prefetch,
                                       value_prefetcher<typename P::value_type>,
                                       do_nothing>::type type;
};

template <typename P>
class node_base : public make_nodeversion<P>::type {
  public:
    static constexpr bool concurrent = P::concurrent;
    static constexpr int nikey = 1;
    typedef leaf<P> leaf_type;
    typedef internode<P> internode_type;
    typedef node_base<P> base_type;
    typedef typename P::value_type value_type;
    typedef leafvalue<P> leafvalue_type;
    typedef typename P::ikey_type ikey_type;
    typedef key<ikey_type> key_type;
    typedef typename make_nodeversion<P>::type nodeversion_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef sidle::node_mem_type node_mem_type_t;

    static sidle::sidle_strategy* strategy_manager;
#ifdef CAL_NODE_HOTNESS
    static std::unordered_map<uint64_t, uint16_t> hotness_map_;
#endif

    node_base(bool isleaf, dsidle::NodeRef control_ref = {})
        : nodeversion_type(isleaf, control_ref), control_ref_(control_ref) {
    }

    node_base(bool isleaf, node_mem_type_t node_type, uint8_t depth, uint16_t time = 1,
              dsidle::NodeRef control_ref = {})
        : nodeversion_type(isleaf, control_ref), control_ref_(control_ref) {}

    dsidle::NodeRef control_ref() const { return control_ref_; }

    node_mem_type_t replica_policy_type() const {
        if (!control_ref_)
            return node_mem_type_t::unknown;
        if (auto* replicas = dsidle::CurrentReplicaDirectoryOrNull()) {
            if (replicas->HasLocalPlacement(
                    control_ref_, dsidle::LoadNodeGeneration(control_ref_)))
                return node_mem_type_t::local;
        }
        return node_mem_type_t::remote;
    }


    inline base_type* parent() const {
        if (control_ref_) {
            const auto parent_ref = dsidle::LoadNodeParentRef(control_ref_);
            return dsidle::ResolveCanonicalNode<base_type>(parent_ref);
        }
        // almost always an internode
        if (this->isleaf())
            return static_cast<const leaf_type*>(this)->parent_;
        else
            return static_cast<const internode_type*>(this)->parent_;
    }
    inline bool parent_exists(base_type* p) const {
        return p != nullptr;
    }
    inline bool has_parent() const {
        return parent_exists(parent());
    }
    inline internode_type* locked_parent(threadinfo& ti) const;
    inline void set_parent(base_type* p) {
        if (control_ref_) {
            const auto parent_ref = p ? p->control_ref() : dsidle::NodeRef{};
            if (p && !parent_ref)
                throw std::runtime_error(
                    "canonical node cannot publish a local parent pointer");
            dsidle::StoreNodeParentRef(control_ref_, parent_ref);
        } else if (this->isleaf())
            static_cast<leaf_type*>(this)->parent_ = p;
        else
            static_cast<internode_type*>(this)->parent_ = p;
    }
    inline size_t canonical_allocation_size() const {
        return this->isleaf()
            ? static_cast<const leaf_type*>(this)->allocated_size()
            : sizeof(internode_type);
    }
    inline void publish_body_before_edge() const {
        if (!this->control_ref())
            return;
        const size_t bytes = canonical_allocation_size();
        latency_sim::RecordSwccWrite(this, bytes);
        dsidle::FlushSwccRange(this, bytes);
    }
    inline void make_layer_root() {
        set_parent(nullptr);
        // Split children inherit a locked version and are flushed by the
        // original unlock path. A freshly constructed layer/root is unlocked,
        // so its complete SWCC body must precede publishing root_bit in HWCC.
        if (this->control_ref() && !this->locked())
            publish_body_before_edge();
        this->mark_root();
    }
    inline base_type* maybe_parent() const {
        base_type* x = parent();
        return parent_exists(x) ? x : const_cast<base_type*>(this);
    }

    inline leaf_type* reach_leaf(const key_type& k, nodeversion_type& version,
                                 threadinfo& ti) const;

    void prefetch_full() const {
        for (int i = 0; i < std::min(16 * std::min(P::leaf_width, P::internode_width) + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int depth, int kdepth) const;

#ifdef CAL_NODE_HOTNESS
    void record_access() const {}
#endif

  private:
    dsidle::NodeRef control_ref_{};
};

template <typename P>
sidle::sidle_strategy* node_base<P>::strategy_manager = nullptr;
#ifdef CAL_NODE_HOTNESS
template <typename P>
std::unordered_map<uint64_t, uint16_t> node_base<P>::hotness_map_;
#endif

template <typename P>
class internode : public node_base<P> {
  public:
    static constexpr int width = P::internode_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef sidle::node_mem_type node_mem_type_t;
    typedef sidle::node_metadata node_metadata_t;

    node_metadata_t sidle_meta;
    uint8_t nkeys_;
#ifdef CAL_NODE_HOTNESS
    mutable uint16_t access_count_{0};
#endif
    uint32_t height_;
    ikey_type ikey0_[width];
    node_link<node_base<P> > child_[width + 1];
    node_link<node_base<P> > parent_;
    kvtimestamp_t created_at_[P::debug_level > 0];
    

    internode(uint32_t height)
        : node_base<P>(false), nkeys_(0), height_(height), parent_(), sidle_meta() {
    }

    internode(uint32_t height, node_mem_type_t type, uint8_t depth, dsidle::NodeRef control_ref = {})
        : node_base<P>(false, type, depth, 1, control_ref), nkeys_(0), height_(height), parent_(), sidle_meta(type, depth) {}

    static internode<P>* make(uint32_t height, threadinfo& ti) {
        void* ptr = ti.pool_allocate(sizeof(internode<P>),
                                     memtag_masstree_internode);
        internode<P>* n = new(ptr) internode<P>(height, node_mem_type_t::local, 0, 1);
        assert(n);
        if (P::debug_level > 0)
            n->created_at_[0] = ti.operation_timestamp();
        return n;
    }

    static internode<P>* make_with_cxl_policy(uint32_t height, threadinfo& ti, uint8_t depth, node_mem_type_t type, bool is_migration = false) {
        // dsidle: the canonical tree is always SWCC; SIDLE no longer decides
        // canonical placement (it will select local replicas in M5).
        (void) type;
        (void) is_migration;
        void* ptr = ti.pool_allocate(sizeof(internode<P>), memtag_masstree_internode_remote);
        canonical_allocation_guard<threadinfo> allocation(
            ti, ptr, sizeof(internode<P>), memtag_masstree_internode_remote);
        auto& pool = dsidle::CurrentSharedPool();
        dsidle::NodeControlSlab controls(pool);
        const dsidle::SwccOffset<std::byte> offset(
            reinterpret_cast<std::byte*>(ptr) - static_cast<std::byte*>(pool.base()));
        const auto ref =
            controls.Reserve(offset.value(), 1, sizeof(internode<P>));
        allocation.pair(controls, ref);
        internode<P>* n = new(ptr) internode<P>(height, node_mem_type_t::remote, depth, ref);
        assert(n);
        if (P::debug_level > 0) {
            n->created_at_[0] = ti.operation_timestamp();
        }
        latency_sim::RecordSwccWrite(n, sizeof(*n));
        dsidle::FlushSwccRange(n, sizeof(*n));
        controls.Publish(ref, 0);
        allocation.commit();
#ifdef CAL_NODE_HOTNESS
        node_base<P>::hotness_map_[reinterpret_cast<uint64_t>(n)] = 0;
#endif
        return n;
    }

    int size() const {
        return nkeys_;
    }

    key_type get_key(int p) const {
        return key_type(ikey0_[p]);
    }
    ikey_type ikey(int p) const {
        return ikey0_[p];
    }
    int compare_key(ikey_type a, int bp) const {
        return ::compare(a, ikey(bp));
    }
    int compare_key(const key_type& a, int bp) const {
        return ::compare(a.ikey(), ikey(bp));
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    void prefetch() const {
        for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
    }

    void print(FILE* f, const char* prefix, int depth, int kdepth) const;

    void deallocate(threadinfo& ti) {
        if (this->sidle_meta.type == node_mem_type_t::local) {
            ti.pool_deallocate(this, sizeof(*this), memtag_masstree_internode);
        } else {
            ti.pool_deallocate(this, sizeof(*this), memtag_masstree_internode_remote);
        }
    }
    void deallocate_rcu(threadinfo& ti) {
        if (this->sidle_meta.type == node_mem_type_t::local) {
            ti.pool_deallocate_rcu(this, sizeof(*this), memtag_masstree_internode, this->control_ref());
        } else {
            ti.pool_deallocate_rcu(this, sizeof(*this), memtag_masstree_internode_remote, this->control_ref());
        }
    }

    void assign_copy(int p, node_base<P>* child) {
        child->set_parent(this);
        child_[p + 1] = child;
    }

#ifdef CAL_NODE_HOTNESS
    void record_access() const {
        if (this->control_ref()) {
            if (auto* replicas = dsidle::CurrentReplicaDirectoryOrNull()) {
                replicas->RecordAccess(this->control_ref());
                return;
            }
        }
        ++access_count_;
        node_base<P>::hotness_map_[reinterpret_cast<uint64_t>(this)] = access_count_;
    }
#endif

  private:
    void assign(int p, ikey_type ikey, node_base<P>* child) {
        child->set_parent(this);
        child_[p + 1] = child;
        ikey0_[p] = ikey;
    }

    void shift_from(int p, const internode<P>* x, int xp, int n) {
        masstree_precondition(x != this);
        if (n) {
            memcpy(ikey0_ + p, x->ikey0_ + xp, sizeof(ikey0_[0]) * n);
            memcpy(child_ + p + 1, x->child_ + xp + 1, sizeof(child_[0]) * n);
        }
    }
    void shift_up(int p, int xp, int n) {
        memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
        for (auto* a = child_ + p + n, *b = child_ + xp + n; n; --a, --b, --n)
            *a = *b;
    }
    void shift_down(int p, int xp, int n) {
        memmove(ikey0_ + p, ikey0_ + xp, sizeof(ikey0_[0]) * n);
        for (auto* a = child_ + p + 1, *b = child_ + xp + 1; n; ++a, ++b, --n)
            *a = *b;
    }

    int split_into(internode<P>* nr, int p, ikey_type ka, node_base<P>* value,
                   ikey_type& split_ikey, int split_type);

    template <typename PP> friend class tcursor;
};

template <typename P>
class leafvalue {
  public:
    typedef typename P::value_type value_type;
    typedef typename make_prefetcher<P>::type prefetcher_type;
    typedef typename std::remove_pointer<value_type>::type value_element_type;

    leafvalue() = default;
    leafvalue(value_type v) {
        raw_ = v ? dsidle::SwccOffset<value_element_type>(
            reinterpret_cast<std::byte*>(v) - static_cast<std::byte*>(dsidle::SharedPoolBase())).value() : 0;
    }
    leafvalue(node_base<P>* n) {
        raw_ = n ? n->control_ref().value() : 0;
    }

    static leafvalue<P> make_empty() {
        return leafvalue<P>(value_type());
    }

    typedef bool (leafvalue<P>::*unspecified_bool_type)() const;
    operator unspecified_bool_type() const {
        return raw_ ? &leafvalue<P>::empty : 0;
    }
    bool empty() const {
        return !raw_;
    }

    value_type value() const {
        auto* value = dsidle::SwccOffset<value_element_type>(raw_).get(dsidle::SharedPoolBase());
        if (value) {
            dsidle::InvalidateSwccRange(value, dsidle::kSwccCacheLineBytes);
            const auto metadata_bytes =
                sizeof(kvtimestamp_t) +
                sizeof(typename value_element_type::offset_type) *
                    (static_cast<size_t>(value->ncol()) + 2);
            if (metadata_bytes > dsidle::kSwccCacheLineBytes) {
                dsidle::InvalidateSwccRange(
                    reinterpret_cast<const std::byte*>(value) +
                        dsidle::kSwccCacheLineBytes,
                    metadata_bytes - dsidle::kSwccCacheLineBytes);
            }
            const auto bytes = value->size();
            const auto visible_bytes =
                std::max(metadata_bytes, dsidle::kSwccCacheLineBytes);
            if (bytes > visible_bytes) {
                dsidle::InvalidateSwccRange(
                    reinterpret_cast<const std::byte*>(value) + visible_bytes,
                    bytes - visible_bytes);
            }
            latency_sim::RecordSwccRead(value, bytes);
        }
        return value;
    }
    void set_value(value_type value) {
        if (value) {
            latency_sim::RecordSwccWrite(value, value->size());
            dsidle::FlushSwccRange(value, value->size());
        }
        raw_ = value ? dsidle::SwccOffset<value_element_type>(
            reinterpret_cast<std::byte*>(value) - static_cast<std::byte*>(dsidle::SharedPoolBase())).value() : 0;
    }

    node_base<P>* layer() const {
        return dsidle::ResolveCanonicalNode<node_base<P> >(dsidle::NodeRef(raw_));
    }
    dsidle::NodeRef layer_ref() const {
        return dsidle::NodeRef(raw_);
    }

    void prefetch(int keylenx) const {
        if (!leaf<P>::keylenx_is_layer(keylenx)) {
            // Preserve upstream Masstree's CPU prefetch without turning it
            // into an eager SWCC visibility read. The later, version-checked
            // value() call performs the one required invalidate/accounting
            // operation; a replica hit need not touch canonical value bytes.
            auto* opaque = dsidle::SwccOffset<value_element_type>(raw_).get(
                dsidle::SharedPoolBase());
            if (opaque)
                prefetcher_type()(opaque);
        }
        // A layer edge stores NodeRef rather than a process-local address.
        // Resolving it here would eagerly invalidate the next canonical node;
        // the normal descent path (or its local replica) resolves it once.
    }

  private:
    std::uint64_t raw_{};
};

template <typename P>
class leaf : public node_base<P> {
  public:
    static constexpr int width = P::leaf_width;
    typedef typename node_base<P>::nodeversion_type nodeversion_type;
    typedef key<typename P::ikey_type> key_type;
    typedef typename node_base<P>::leafvalue_type leafvalue_type;
    typedef kpermuter<P::leaf_width> permuter_type;
    typedef typename P::ikey_type ikey_type;
    typedef typename key_bound<width, P::bound_method>::type bound_type;
    typedef typename P::threadinfo_type threadinfo;
    typedef stringbag<uint8_t> internal_ksuf_type;
    typedef stringbag<uint16_t> external_ksuf_type;
    typedef typename P::phantom_epoch_type phantom_epoch_type;
    typedef sidle::node_mem_type node_mem_type_t;
    typedef sidle::node_metadata node_metadata_t;
    typedef sidle::leaf_metadata leaf_metadata_t;
    static constexpr int ksuf_keylenx = 64;
    static constexpr int layer_keylenx = 128;

    enum {
        modstate_insert = 0, modstate_remove = 1, modstate_deleted_layer = 2
    };

    leaf_metadata_t sidle_meta;
    int8_t extrasize64_;
    uint8_t modstate_;
    uint8_t keylenx_[width];
    typename permuter_type::storage_type permutation_;
    ikey_type ikey0_[width];
    leafvalue_type lv_[width];
    swcc_link<external_ksuf_type> ksuf_;
    tagged_node_link<leaf<P> > next_;
    node_link<leaf<P> > prev_;
    node_link<node_base<P> > parent_;
    phantom_epoch_type phantom_epoch_[P::need_phantom_epoch];
    kvtimestamp_t created_at_[P::debug_level > 0];
    internal_ksuf_type iksuf_[0];
    

    leaf(size_t sz, phantom_epoch_type phantom_epoch, node_mem_type_t type = node_mem_type_t::unknown, uint8_t depth = 0, uint16_t access_time = 1, dsidle::NodeRef control_ref = {})
        : node_base<P>(true, type, depth, access_time, control_ref), modstate_(modstate_insert),
          permutation_(permuter_type::make_empty()),
          ksuf_(), parent_(), iksuf_{}, sidle_meta(type, depth, access_time) {
        masstree_precondition(sz % 64 == 0 && sz / 64 < 128);
        extrasize64_ = (int(sz) >> 6) - ((int(sizeof(*this)) + 63) >> 6);
        if (extrasize64_ > 0) {
            new((void*) &iksuf_[0]) internal_ksuf_type(width, sz - sizeof(*this));
        }
        if (P::need_phantom_epoch) {
            if (this->control_ref())
                dsidle::StoreNodePhantomEpoch(
                    this->control_ref(), phantom_epoch);
            else
                phantom_epoch_[0] = phantom_epoch;
        }
    }


    static leaf<P>* make(int ksufsize, phantom_epoch_type phantom_epoch, threadinfo& ti) {
        size_t sz = iceil(sizeof(leaf<P>) + std::min(ksufsize, 128), 64);
        void* ptr = ti.pool_allocate(sz, memtag_masstree_leaf);
        leaf<P>* n = new(ptr) leaf<P>(sz, phantom_epoch, node_mem_type_t::local, 0, 1);
        assert(n);
        if (P::debug_level > 0) {
            n->created_at_[0] = ti.operation_timestamp();
        }
        return n;
    }

    static leaf<P>* make_with_cxl_policy(int ksufsize, phantom_epoch_type phantom_epoch, threadinfo& ti, uint8_t depth, node_mem_type_t type, uint16_t access_time = 1, bool is_migration = false) {
        size_t sz = iceil(sizeof(leaf<P>) + std::min(ksufsize, 128), 64);
        (void) type;
        (void) is_migration;
        void* ptr = ti.pool_allocate(sz, memtag_masstree_leaf_remote);
        canonical_allocation_guard<threadinfo> allocation(
            ti, ptr, sz, memtag_masstree_leaf_remote);
        auto& pool = dsidle::CurrentSharedPool();
        dsidle::NodeControlSlab controls(pool);
        const dsidle::SwccOffset<std::byte> offset(
            reinterpret_cast<std::byte*>(ptr) - static_cast<std::byte*>(pool.base()));
        const auto ref = controls.Reserve(offset.value(), 2, sz);
        allocation.pair(controls, ref);
        leaf<P>* n = new(ptr) leaf<P>(sz, phantom_epoch, node_mem_type_t::remote, depth, access_time, ref);
        assert(n);
        if (P::debug_level > 0) {
            n->created_at_[0] = ti.operation_timestamp();
        }
        latency_sim::RecordSwccWrite(n, sz);
        dsidle::FlushSwccRange(n, sz);
        controls.Publish(ref, dsidle::MasstreeNodeVersionBits::isleaf_bit);
        allocation.commit();
#ifdef CAL_NODE_HOTNESS
        node_base<P>::hotness_map_[reinterpret_cast<uint64_t>(n)] = 0;
#endif
        return n;
    }

    static leaf<P>* make_root(int ksufsize, leaf<P>* parent, threadinfo& ti,
                              bool* initial_local = nullptr,
                              node_mem_type_t parent_policy_type =
                                  node_mem_type_t::unknown) {
        leaf<P>* n = nullptr;
        uint8_t depth = 1;
        node_mem_type_t new_node_type = node_mem_type_t::local;
        if (parent) {
            depth = parent->sidle_meta.metadata.depth + 1;
            if (parent_policy_type == node_mem_type_t::unknown)
                parent_policy_type = parent->replica_policy_type();
            new_node_type =
                node_base<P>::strategy_manager->decide_new_node_position(
                    parent_policy_type, depth);
        }
        if (initial_local)
            *initial_local = new_node_type == node_mem_type_t::local;
        n = make_with_cxl_policy(ksufsize, parent ? parent->phantom_epoch() : phantom_epoch_type(), ti, 1, new_node_type, 1, false);
        n->next_ = nullptr;
        n->prev_ = nullptr;
        n->ikey0_[0] = 0; // to avoid undefined behavior
        n->make_layer_root();
        return n;
    }

    inline void record_access() {
        if (this->control_ref()) {
            if (auto* replicas = dsidle::CurrentReplicaDirectoryOrNull()) {
                replicas->RecordAccess(this->control_ref());
                return;
            }
        }
        ++sidle_meta.access_time;
#ifdef CAL_NODE_HOTNESS
        node_base<P>::hotness_map_[reinterpret_cast<uint64_t>(this)] = sidle_meta.access_time;
#endif
    }

    static size_t min_allocated_size() {
        return (sizeof(leaf<P>) + 63) & ~size_t(63);
    }
    size_t allocated_size() const {
        int es = (extrasize64_ >= 0 ? extrasize64_ : -extrasize64_ - 1);
        return (sizeof(*this) + es * 64 + 63) & ~size_t(63);
    }
    phantom_epoch_type phantom_epoch() const {
        if (!P::need_phantom_epoch)
            return phantom_epoch_type();
        return this->control_ref()
            ? static_cast<phantom_epoch_type>(
                  dsidle::LoadNodePhantomEpoch(this->control_ref()))
            : phantom_epoch_[0];
    }
    void set_phantom_epoch(phantom_epoch_type epoch) {
        if (!P::need_phantom_epoch)
            return;
        if (this->control_ref())
            dsidle::StoreNodePhantomEpoch(this->control_ref(), epoch);
        else
            phantom_epoch_[0] = epoch;
    }
    void raise_phantom_epoch(phantom_epoch_type epoch) {
        if (!P::need_phantom_epoch)
            return;
        auto current = phantom_epoch();
        while (circular_int<phantom_epoch_type>::less(current, epoch)) {
            if (!this->control_ref()) {
                phantom_epoch_[0] = epoch;
                return;
            }
            std::uint64_t expected = current;
            if (dsidle::CompareExchangeNodePhantomEpoch(
                    this->control_ref(), &expected, epoch))
                return;
            current = static_cast<phantom_epoch_type>(expected);
        }
    }

    int size() const {
        return permuter_type::size(permutation_);
    }
    permuter_type permutation() const {
        return permuter_type(permutation_);
    }
    typename nodeversion_type::value_type full_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        return (this->version_value() << permuter_type::size_bits) + size();
    }
    typename nodeversion_type::value_type full_unlocked_version_value() const {
        static_assert(int(nodeversion_type::traits_type::top_stable_bits) >= int(permuter_type::size_bits), "not enough bits to add size to version");
        typename node_base<P>::nodeversion_type v(*this);
        if (v.locked()) {
            // subtly, unlocked_version_value() is different than v.unlock();
            // v.version_value() because the latter will add a split bit if
            // we're doing a split. So we do the latter to get the fully
            // correct version.
            v.unlock();
        }
        return (v.version_value() << permuter_type::size_bits) + size();
    }

    using node_base<P>::has_changed;
    bool has_changed(nodeversion_type oldv,
                     typename permuter_type::storage_type oldperm) const {
        return this->has_changed(oldv) || oldperm != permutation_;
    }

    key_type get_key(int p) const {
        int keylenx = keylenx_[p];
        if (!keylenx_has_ksuf(keylenx))
            return key_type(ikey0_[p], keylenx);
        else
            return key_type(ikey0_[p], ksuf(p));
    }
    ikey_type ikey(int p) const {
        return ikey0_[p];
    }
    ikey_type ikey_bound() const {
        return ikey0_[0];
    }
    int compare_key(const key_type& a, int bp) const {
        return a.compare(ikey(bp), keylenx_[bp]);
    }
    inline int stable_last_key_compare(const key_type& k, nodeversion_type v,
                                       threadinfo& ti) const;

    inline leaf<P>* advance_to_key(const key_type& k, nodeversion_type& version,
                                   threadinfo& ti) const;

    static bool keylenx_is_layer(int keylenx) {
        return keylenx > 127;
    }
    static bool keylenx_has_ksuf(int keylenx) {
        return keylenx == ksuf_keylenx;
    }

    bool is_layer(int p) const {
        return keylenx_is_layer(keylenx_[p]);
    }
    bool has_ksuf(int p) const {
        return keylenx_has_ksuf(keylenx_[p]);
    }
    Str ksuf(int p, int keylenx) const {
        (void) keylenx;
        masstree_precondition(keylenx_has_ksuf(keylenx));
        external_ksuf_type* external = readable_external_ksuf();
        return external ? readable_external_ksuf_value(external, p)
                        : iksuf_[0].get(p);
    }
    Str ksuf(int p) const {
        return ksuf(p, keylenx_[p]);
    }
    bool ksuf_equals(int p, const key_type& ka) const {
        return ksuf_equals(p, ka, keylenx_[p]);
    }
    bool ksuf_equals(int p, const key_type& ka, int keylenx) const {
        if (!keylenx_has_ksuf(keylenx))
            return true;
        Str s = ksuf(p, keylenx);
        return s.len == ka.suffix().len
            && string_slice<uintptr_t>::equals_sloppy(s.s, ka.suffix().s, s.len);
    }
    // Returns 1 if match & not layer, 0 if no match, <0 if match and layer
    int ksuf_matches(int p, const key_type& ka) const {
        int keylenx = keylenx_[p];
        if (keylenx < ksuf_keylenx)
            return 1;
        if (keylenx == layer_keylenx)
            return -(int) sizeof(ikey_type);
        Str s = ksuf(p, keylenx);
        return s.len == ka.suffix().len
            && string_slice<uintptr_t>::equals_sloppy(s.s, ka.suffix().s, s.len);
    }
    int ksuf_compare(int p, const key_type& ka) const {
        int keylenx = keylenx_[p];
        if (!keylenx_has_ksuf(keylenx))
            return 0;
        return ksuf(p, keylenx).compare(ka.suffix());
    }

    size_t ksuf_used_capacity() const {
        if (external_ksuf_type* external = readable_external_ksuf())
            return external->used_capacity();
        else if (extrasize64_ > 0)
            return iksuf_[0].used_capacity();
        else
            return 0;
    }
    size_t ksuf_capacity() const {
        if (external_ksuf_type* external = readable_external_ksuf())
            return external->capacity();
        else if (extrasize64_ > 0)
            return iksuf_[0].capacity();
        else
            return 0;
    }
    bool ksuf_external() const {
        return ksuf_;
    }
    Str ksuf_storage(int p) const {
        if (external_ksuf_type* external = readable_external_ksuf())
            return readable_external_ksuf_value(external, p);
        else if (extrasize64_ > 0)
            return iksuf_[0].get(p);
        else
            return Str();
    }

    bool deleted_layer() const {
        return modstate_ == modstate_deleted_layer;
    }

    void prefetch() const {
        for (int i = 64; i < std::min(16 * width + 1, 4 * 64); i += 64)
            ::prefetch((const char *) this + i);
        if (extrasize64_ > 0)
            ::prefetch((const char *) &iksuf_[0]);
        else if (extrasize64_ < 0) {
            const auto* external = static_cast<external_ksuf_type*>(ksuf_);
            ::prefetch((const char *) external);
            ::prefetch((const char *) external + CACHE_LINE_SIZE);
        }
    }

    void print(FILE* f, const char* prefix, int depth, int kdepth) const;

    leaf<P>* safe_next() const {
        dsidle::InvalidateSwccRange(&next_, sizeof(next_));
        latency_sim::RecordSwccRead(&next_, sizeof(next_));
        return next_;
    }
    leaf<P>* safe_prev() const {
        dsidle::InvalidateSwccRange(&prev_, sizeof(prev_));
        latency_sim::RecordSwccRead(&prev_, sizeof(prev_));
        return prev_;
    }

    void deallocate(threadinfo& ti) {
        if (external_ksuf_type* external = readable_external_ksuf())
            ti.deallocate(external, external->capacity(),
                          memtag_masstree_ksuffixes);
        if (extrasize64_ != 0)
            iksuf_[0].~stringbag(); 
        if (this->sidle_meta.metadata.type == node_mem_type_t::local) {
            ti.pool_deallocate(this, allocated_size(), memtag_masstree_leaf);
        } else {
            ti.pool_deallocate(this, allocated_size(), memtag_masstree_leaf_remote);
        }
    }
    void deallocate_rcu(threadinfo& ti) {
        if (external_ksuf_type* external = readable_external_ksuf())
            ti.deallocate_rcu(external, external->capacity(),
                              memtag_masstree_ksuffixes); 
        if (this->sidle_meta.metadata.type == node_mem_type_t::local) {
            ti.pool_deallocate_rcu(this, allocated_size(), memtag_masstree_leaf, this->control_ref());
        } else {
            ti.pool_deallocate_rcu(this, allocated_size(), memtag_masstree_leaf_remote, this->control_ref());
        }
    }

  private:
    external_ksuf_type* readable_external_ksuf() const {
        external_ksuf_type* external = ksuf_;
        if (!external)
            return nullptr;
        const size_t metadata_bytes = external_ksuf_type::overhead(width);
        dsidle::InvalidateSwccRange(external, metadata_bytes);
        latency_sim::RecordSwccRead(external, metadata_bytes);
        return external;
    }
    static Str readable_external_ksuf_value(external_ksuf_type* external,
                                            int p) {
        const Str suffix = external->get(p);
        if (suffix.len) {
            dsidle::InvalidateSwccRange(suffix.s, suffix.len);
            latency_sim::RecordSwccRead(suffix.s, suffix.len);
        }
        return suffix;
    }

    inline void mark_deleted_layer() {
        modstate_ = modstate_deleted_layer;
    }

    inline void assign(int p, const key_type& ka, threadinfo& ti) {
        lv_[p] = leafvalue_type::make_empty();
        ikey0_[p] = ka.ikey();
        if (!ka.has_suffix()) {
            keylenx_[p] = ka.length();
        } else {
            keylenx_[p] = ksuf_keylenx;
            assign_ksuf(p, ka.suffix(), false, ti);
        }
    }

    template <typename PP> friend class leaf_replica;
    inline void assign_initialize(int p, const key_type& ka, threadinfo& ti) {
        lv_[p] = leafvalue_type::make_empty();
        ikey0_[p] = ka.ikey();
        if (!ka.has_suffix()) {
            keylenx_[p] = ka.length();
        } else {
            keylenx_[p] = ksuf_keylenx;
            assign_ksuf(p, ka.suffix(), true, ti);
        }
    }
    inline void assign_initialize(int p, leaf<P>* x, int xp, threadinfo& ti) {
        lv_[p] = x->lv_[xp];
        ikey0_[p] = x->ikey0_[xp];
        keylenx_[p] = x->keylenx_[xp];
        if (x->has_ksuf(xp)) {
            assign_ksuf(p, x->ksuf(xp), true, ti);
        }
    }
    inline void assign_initialize_for_layer(int p, const key_type& ka) {
        assert(ka.has_suffix());
        ikey0_[p] = ka.ikey();
        keylenx_[p] = layer_keylenx;
    }
    void assign_ksuf(int p, Str s, bool initializing, threadinfo& ti);

    inline ikey_type ikey_after_insert(const permuter_type& perm, int i,
                                       const tcursor<P>* cursor) const;
    int split_into(leaf<P>* nr, tcursor<P>* tcursor, ikey_type& split_ikey,
                   threadinfo& ti);

    template <typename PP> friend class tcursor;
};


template <typename P>
void basic_table<P>::initialize(threadinfo& ti, const int cxl_percentage) {
    #ifdef CXL
    cxl_init(CXL_MAX_SIZE, cxl_percentage);
    #endif
    masstree_precondition(!root_ref_);
    node_type* root = node_type::leaf_type::make_root(0, 0, ti);
    root_ref_ = root->control_ref();
    dsidle::RootControlAccessor(
        dsidle::CurrentSharedPool().root_control()).publish(
            root_ref_, dsidle::LoadNodeGeneration(root_ref_));
}

template <typename P>
void basic_table<P>::attach() {
    masstree_precondition(!root_ref_);
    const auto root = dsidle::RootControlAccessor(dsidle::CurrentSharedPool().root_control()).stable();
    if (!root.ref)
        throw std::runtime_error("D-SIDLE pool has no published Masstree root");
    if (dsidle::LoadNodeAllocationState(root.ref) !=
            dsidle::NodeAllocationState::kPublished ||
        dsidle::LoadNodeGeneration(root.ref) != root.generation)
        throw std::runtime_error("D-SIDLE RootControl generation does not match NodeControl");
    root_ref_ = root.ref;
}


/** @brief Return this node's parent in locked state.
    @pre this->locked()
    @post this->parent() == result && (!result || result->locked()) */
template <typename P>
internode<P>* node_base<P>::locked_parent(threadinfo& ti) const
{
    node_base<P>* p;
    masstree_precondition(!this->concurrent || this->locked());
    while (true) {
        p = this->parent();
        if (!this->parent_exists(p)) {
            break;
        }
        nodeversion_type pv = p->lock(*p, ti.lock_fence(tc_internode_lock));
        if (p == this->parent()) {
            masstree_invariant(!p->isleaf());
            break;
        }
        p->unlock(pv);
        relax_fence();
    }
    return static_cast<internode<P>*>(p);
}


template <typename P>
void node_base<P>::print(FILE* f, const char* prefix, int depth, int kdepth) const
{
    if (this->isleaf())
        static_cast<const leaf<P>*>(this)->print(f, prefix, depth, kdepth);
    else
        static_cast<const internode<P>*>(this)->print(f, prefix, depth, kdepth);
}


/** @brief Return the result of compare_key(k, LAST KEY IN NODE).

    Reruns the comparison until a stable comparison is obtained. */
template <typename P>
inline int
internode<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                      threadinfo& ti) const
{
    while (true) {
        int n = this->size();
        int cmp = n ? compare_key(k, n - 1) : 1;
        if (likely(!this->has_changed(v))) {
            return cmp;
        }
        v = this->stable_annotated(ti.stable_fence());
    }
}

template <typename P>
inline int
leaf<P>::stable_last_key_compare(const key_type& k, nodeversion_type v,
                                 threadinfo& ti) const
{
    while (true) {
        typename leaf<P>::permuter_type perm(permutation_);
        int n = perm.size();
        // If `n == 0`, then this node is empty: it was deleted without ever
        // splitting, or it split and then was emptied.
        // - It is always safe to return 1, because then the caller will
        //   check more precisely whether `k` belongs in `this`.
        // - It is safe to return anything if `this->deleted()`, because
        //   viewing the deleted node will always cause a retry.
        // - Thus it is safe to return a comparison with the key stored in slot
        //   `perm[0]`. If the node ever had keys in it, then kpermuter ensures
        //   that slot holds the most recently deleted key, which would belong
        //   in this leaf. Otherwise, `perm[0]` is 0.
        int p = perm[n ? n - 1 : 0];
        int cmp = compare_key(k, p);
        if (likely(!this->has_changed(v))) {
            return cmp;
        }
        v = this->stable_annotated(ti.stable_fence());
    }
}


/** @brief Return the leaf in this tree layer responsible for @a ka.

    Returns a stable leaf. Sets @a version to the stable version. */
template <typename P>
inline leaf<P>* node_base<P>::reach_leaf(const key_type& ka,
                                         nodeversion_type& version,
                                         threadinfo& ti) const
{
    const node_base<P> *n[2];
    typename node_base<P>::nodeversion_type v[2];
    unsigned sense;

    // Get a non-stale root.
    // Detect staleness by checking whether n has ever split.
    // The true root has never split.
 retry:
    sense = 0;
    n[sense] = this;
#ifdef CAL_NODE_HOTNESS
    n[sense]->record_access();
#endif
    while (true) {
        v[sense] = n[sense]->stable_annotated(ti.stable_fence());
        if (v[sense].is_root()) {
            break;
        }
        ti.mark(tc_root_retry);
        n[sense] = n[sense]->maybe_parent();
#ifdef CAL_NODE_HOTNESS
        n[sense]->record_access();
#endif
    }

    // Loop over internal nodes.
    while (!v[sense].isleaf()) {
        const internode<P> *in = static_cast<const internode<P>*>(n[sense]);
        in->prefetch();
        node_base<P>* child = nullptr;
        if (auto* replicas = dsidle::CurrentReplicaDirectoryOrNull()) {
            const auto ref = in->control_ref();
            const auto generation = dsidle::LoadNodeGeneration(ref);
            auto handle = replicas->Acquire(ref, generation, v[sense].version_value());
            if (handle && handle.snapshot().kind == dsidle::ReplicaKind::kInternal) {
                const auto child_ref = internode_replica<P>::LookupChild(handle.snapshot().local_ptr, ka);
                child = dsidle::ResolveCanonicalNode<node_base<P>>(child_ref);
                replicas->RecordInternalHit();
            }
        }
        if (!child) {
            int kp = internode<P>::bound_type::upper(ka, *in);
            child = in->child_[kp];
        }
        n[sense ^ 1] = child;
#ifdef CAL_NODE_HOTNESS
        n[sense ^ 1]->record_access();
#endif
        if (!child) {
            goto retry;
        }
        v[sense ^ 1] = n[sense ^ 1]->stable_annotated(ti.stable_fence());

        if (likely(!in->has_changed(v[sense]))) {
            sense ^= 1;
            continue;
        }

        typename node_base<P>::nodeversion_type oldv = v[sense];
        v[sense] = in->stable_annotated(ti.stable_fence());
        if (unlikely(oldv.has_split(v[sense]))
            && in->stable_last_key_compare(ka, v[sense], ti) > 0) {
            ti.mark(tc_root_retry);
            goto retry;
        } else {
            ti.mark(tc_internode_retry);
        }
    }

    version = v[sense];
    auto result = const_cast<leaf<P> *>(static_cast<const leaf<P> *>(n[sense]));
    result->record_access();
    return result;
}

/** @brief Return the leaf at or after *this responsible for @a ka.
    @pre *this was responsible for @a ka at version @a v

    Checks whether *this has split since version @a v. If it has split, then
    advances through the leaves using the B^link-tree pointers and returns
    the relevant leaf, setting @a v to the stable version for that leaf. */
template <typename P>
leaf<P>* leaf<P>::advance_to_key(const key_type& ka, nodeversion_type& v,
                                 threadinfo& ti) const
{
    const leaf<P>* n = this;
    nodeversion_type oldv = v;
    v = n->stable_annotated(ti.stable_fence());
    if (unlikely(v.has_split(oldv))
        && n->stable_last_key_compare(ka, v, ti) > 0) {
        leaf<P> *next;
        ti.mark(tc_leaf_walk);
        while (likely(!v.deleted())
               && (next = n->safe_next())
               && compare(ka.ikey(), next->ikey_bound()) >= 0) {
            n = next;
            v = n->stable_annotated(ti.stable_fence());
        }
    }
    return const_cast<leaf<P>*>(n);
}


/** @brief Assign position @a p's keysuffix to @a s.

    This may allocate a new suffix container, copying suffixes over.

    The @a initializing parameter determines which suffixes are copied. If @a
    initializing is false, then this is an insertion into a live node. The
    live node's permutation indicates which keysuffixes are active, and only
    active suffixes are copied. If @a initializing is true, then this
    assignment is part of the initialization process for a new node. The
    permutation might not be set up yet. In this case, it is assumed that key
    positions [0,p) are ready: keysuffixes in that range are copied. In either
    case, the key at position p is NOT copied; it is assigned to @a s. */
template <typename P>
void leaf<P>::assign_ksuf(int p, Str s, bool initializing, threadinfo& ti) {
    external_ksuf_type* oksuf = readable_external_ksuf();
    if (!oksuf && extrasize64_ > 0 && iksuf_[0].assign(p, s))
        return;

    permuter_type perm(permutation_);
    int n = initializing ? p : perm.size();

    size_t csz = 0;
    std::array<Str, width> old_suffixes{};
    for (int i = 0; i < n; ++i) {
        int mp = initializing ? i : perm[i];
        if (mp != p && has_ksuf(mp)) {
            old_suffixes[mp] =
                oksuf ? readable_external_ksuf_value(oksuf, mp)
                      : iksuf_[0].get(mp);
            csz += old_suffixes[mp].len;
        }
    }

    size_t sz = iceil_log2(external_ksuf_type::safe_size(width, csz + s.len));
    if (oksuf)
        sz = std::max(sz, oksuf->capacity());

    void* ptr = ti.allocate(sz, memtag_masstree_ksuffixes);
    external_ksuf_type* nksuf = new(ptr) external_ksuf_type(width, sz);
    for (int i = 0; i < n; ++i) {
        int mp = initializing ? i : perm[i];
        if (mp != p && has_ksuf(mp)) {
            bool ok = nksuf->assign(mp, old_suffixes[mp]);
            assert(ok); (void) ok;
        }
    }
    bool ok = nksuf->assign(p, s);
    assert(ok); (void) ok;
    latency_sim::RecordSwccWrite(nksuf, sz);
    dsidle::FlushSwccRange(nksuf, sz);
    fence();

    // removed ksufs aren't copied to the new ksuf, but observers
    // might need them. We ensure that observers must retry by
    // ensuring that we are not currently in the remove state.
    // State transitions are accompanied by mark_insert() so observers
    // will retry.
    masstree_invariant(modstate_ != modstate_remove);

    ksuf_ = nksuf;
    fence();

    if (extrasize64_ >= 0)      // now the new ksuf_ installed, mark old dead
        extrasize64_ = -extrasize64_ - 1;

    if (oksuf)
        ti.deallocate_rcu(oksuf, oksuf->capacity(),
                          memtag_masstree_ksuffixes);
}

template <typename P>
inline basic_table<P>::basic_table()
    : root_ref_() {
}

template <typename P>
inline node_base<P>* basic_table<P>::root() const {
    return dsidle::ResolveCanonicalNode<node_base<P> >(root_ref_);
}

template <typename P>
inline dsidle::NodeRef basic_table<P>::stable_root_ref() const {
    const auto root =
        dsidle::RootControlAccessor(
            dsidle::CurrentSharedPool().root_control()).stable();
    if (!root.ref)
        throw std::runtime_error("D-SIDLE pool has no published Masstree root");
    return root.ref;
}

template <typename P>
inline node_base<P>* basic_table<P>::fix_root() {
    node_base<P>* root = this->root();
    if (unlikely(!root->is_root())) {
        root = root->maybe_parent();
        root_ref_ = root->control_ref();
        dsidle::RootControlAccessor(
            dsidle::CurrentSharedPool().root_control()).publish(
                root_ref_, dsidle::LoadNodeGeneration(root_ref_));
    }
    return root;
}

} // namespace Masstree
#endif
