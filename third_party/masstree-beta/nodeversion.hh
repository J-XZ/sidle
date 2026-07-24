/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
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
#ifndef MASSTREE_NODEVERSION_HH
#define MASSTREE_NODEVERSION_HH
#include "compiler.hh"
#include "dsidle/shared_pool.h"
#include "dsidle/swcc_visibility.h"

template <typename P>
class nodeversion {
  public:
    typedef P traits_type;
    typedef typename P::value_type value_type;

    nodeversion() = default;
    explicit nodeversion(bool isleaf, dsidle::NodeRef control_ref = {})
        : v_(isleaf ? (value_type) P::isleaf_bit : 0), control_ref_(control_ref) {}
    // Copies are stable stack snapshots, never a second live handle.
    nodeversion(const nodeversion& x) : v_(x.load()) {}

    bool isleaf() const {
        return load() & P::isleaf_bit;
    }

    nodeversion<P> stable() const {
        return stable(relax_fence_function());
    }
    template <typename SF>
    nodeversion<P> stable(SF spin_function) const {
        value_type x = load();
        while (x & P::dirty_mask) {
            spin_function();
            x = load();
        }
        acquire_fence();
        return x;
    }
    template <typename SF>
    nodeversion<P> stable_annotated(SF spin_function) const {
        value_type x = load();
        while (x & P::dirty_mask) {
            spin_function(nodeversion<P>(x));
            x = load();
        }
        acquire_fence();
        return x;
    }

    bool locked() const {
        return load() & P::lock_bit;
    }
    bool inserting() const {
        return load() & P::inserting_bit;
    }
    bool splitting() const {
        return load() & P::splitting_bit;
    }
    bool deleted() const {
        return load() & P::deleted_bit;
    }

    bool migrating() const {
        return load() & P::migration_bit;
    }

    bool has_changed(nodeversion<P> x, bool readonly = true) const {
        fence();
        auto diff = x.v_ ^ load();
        if (readonly) {
            diff &= ~P::migration_bit;
            return diff > P::lock_bit;
        }
        return diff > P::lock_bit;
    }
    bool is_root() const {
        return load() & P::root_bit;
    }
    bool has_split(nodeversion<P> x) const {
        fence();
        auto diff = x.v_ ^ load();
        diff &= ~P::migration_bit;
        return diff >= P::vsplit_lowbit;
    }
    bool simple_has_split(nodeversion<P> x) const {
        auto diff = x.v_ ^ load();
        diff &= ~P::migration_bit;
        return diff >= P::vsplit_lowbit;
    }

    nodeversion<P> lock() {
        return lock(*this);
    }
    nodeversion<P> lock(nodeversion<P> expected) {
        return lock(expected, relax_fence_function());
    }
    template <typename SF>
    nodeversion<P> lock(nodeversion<P> expected, SF spin_function) {
        while (true) {
            if (!(expected.v_ & P::lock_bit)
                && compare_exchange(expected.v_, expected.v_ | P::lock_bit)) {
                break;
            }
            spin_function();
            expected.v_ = load();
        }
        
        masstree_invariant(!(expected.v_ & P::dirty_mask));
        expected.v_ |= P::lock_bit;
        acquire_fence();
        if (expected.v_ != load()) {
            fprintf(stderr, "expected: %u, v_: %u, the address: %p\n", expected.v_, load(), this);
        }
        masstree_invariant(expected.v_ == load());
        return expected;
    }

    bool try_lock() {
        return try_lock(relax_fence_function());
    }
    template <typename SF>
    bool try_lock(SF spin_function) {
        value_type expected = load();
        if (!(expected & P::lock_bit)
            && compare_exchange(expected, expected | P::lock_bit)) {
            masstree_invariant(!(expected & P::dirty_mask));
            acquire_fence();
            masstree_invariant((expected | P::lock_bit) == load());
            return true;
        } else {
            spin_function();
            return false;
        }
    }

    void unlock() {
        unlock(*this);
    }
    void unlock(nodeversion<P> x) {
        const value_type current = load();
        masstree_invariant((fence(), x.v_ == current || x.v_ ^ current == P::migration_bit));
        masstree_invariant(x.v_ & P::lock_bit);
        if (x.v_ & P::splitting_bit)
            x.v_ = (x.v_ + P::vsplit_lowbit) & P::split_unlock_mask;
        else
            x.v_ = (x.v_ + ((x.v_ & P::inserting_bit) << 2)) & P::unlock_mask;
        if (control_ref_) {
            // dsidle: canonical leaf/internode bodies are bounded by the
            // baseline 512B node envelope; publish their SWCC writes before
            // releasing the matching HWCC version.
            auto* control = control_ref_.get(dsidle::SharedPoolBase());
            dsidle::FlushSwccRange(static_cast<std::byte*>(dsidle::SharedPoolBase()) +
                                       control->canonical_swcc_offset, 512);
        }
        release_fence();
        store(x.v_);
        masstree_invariant(!(load() & P::lock_bit));
    }

    void mark_insert() {
        masstree_invariant(locked());
        store(load() | P::inserting_bit);
        acquire_fence();
    }
    nodeversion<P> mark_insert(nodeversion<P> current_version) {
        masstree_invariant((fence(), load() == current_version.v_));
        masstree_invariant(current_version.v_ & P::lock_bit);
        store(current_version.v_ |= P::inserting_bit);
        acquire_fence();
        return current_version;
    }
    void mark_split() {
        masstree_invariant(locked());
        store(load() | P::splitting_bit);
        acquire_fence();
    }
    void mark_change(bool is_split) {
        masstree_invariant(locked());
        store(load() | ((is_split + 1) << P::inserting_shift));
        acquire_fence();
    }
    void mark_migration() {
        store(load() | P::migration_bit);
        acquire_fence();
    }

    void unmark_migation() {
        store(load() & ~P::migration_bit);
        acquire_fence();
    }

    nodeversion<P> mark_deleted() {
        masstree_invariant(locked());
        store(load() | P::deleted_bit | P::splitting_bit);
        acquire_fence();
        return *this;
    }
    void mark_deleted_tree() {
        masstree_invariant(locked() && is_root());
        store(load() | P::deleted_bit);
        acquire_fence();
    }
    void mark_root() {
        store(load() | P::root_bit);
        acquire_fence();
    }
    void mark_nonroot() {
        masstree_invariant(locked());
        store(load() & ~P::root_bit);
        acquire_fence();
    }

    void assign_version(nodeversion<P> x) {
        store(x.v_);
    }

    value_type version_value() const {
        return load();
    }
    value_type unlocked_version_value() const {
        return load() & P::unlock_mask;
    }

  private:
    value_type load() const {
        if (!control_ref_) return v_;
        return static_cast<value_type>(control_ref_.get(dsidle::SharedPoolBase())
            ->version_and_state.load(std::memory_order_acquire));
    }
    void store(value_type value) {
        if (!control_ref_) v_ = value;
        else control_ref_.get(dsidle::SharedPoolBase())->version_and_state.store(value, std::memory_order_release);
    }
    bool compare_exchange(value_type expected, value_type desired) {
        if (!control_ref_) return bool_cmpxchg(&v_, expected, desired);
        std::uint64_t observed = expected;
        return control_ref_.get(dsidle::SharedPoolBase())->version_and_state.compare_exchange_strong(
            observed, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    value_type v_{};
    dsidle::NodeRef control_ref_{};

    nodeversion(value_type v)
        : v_(v) {
    }
};


template <typename P>
class singlethreaded_nodeversion {
  public:
    typedef P traits_type;
    typedef typename P::value_type value_type;

    singlethreaded_nodeversion() {
    }
    explicit singlethreaded_nodeversion(bool isleaf, dsidle::NodeRef = {}) {
        v_ = isleaf ? (value_type) P::isleaf_bit : 0;
    }

    bool isleaf() const {
        return v_ & P::isleaf_bit;
    }

    singlethreaded_nodeversion<P> stable() const {
        return *this;
    }
    template <typename SF>
    singlethreaded_nodeversion<P> stable(SF) const {
        return *this;
    }
    template <typename SF>
    singlethreaded_nodeversion<P> stable_annotated(SF) const {
        return *this;
    }

    bool locked() const {
        return false;
    }
    bool inserting() const {
        return false;
    }
    bool splitting() const {
        return false;
    }
    bool deleted() const {
        return false;
    }
    bool has_changed(singlethreaded_nodeversion<P>) const {
        return false;
    }
    bool is_root() const {
        return v_ & P::root_bit;
    }
    bool has_split(singlethreaded_nodeversion<P>) const {
        return false;
    }
    bool simple_has_split(singlethreaded_nodeversion<P>) const {
        return false;
    }

    singlethreaded_nodeversion<P> lock() {
        return *this;
    }
    singlethreaded_nodeversion<P> lock(singlethreaded_nodeversion<P>) {
        return *this;
    }
    template <typename SF>
    singlethreaded_nodeversion<P> lock(singlethreaded_nodeversion<P>, SF) {
        return *this;
    }

    bool try_lock() {
        return true;
    }
    template <typename SF>
    bool try_lock(SF) {
        return true;
    }

    void unlock() {
    }
    void unlock(singlethreaded_nodeversion<P>) {
    }

    void mark_insert() {
    }
    singlethreaded_nodeversion<P> mark_insert(singlethreaded_nodeversion<P>) {
        return *this;
    }
    void mark_split() {
        v_ &= ~P::root_bit;
    }
    void mark_change(bool is_split) {
        if (is_split)
            mark_split();
    }
    singlethreaded_nodeversion<P> mark_deleted() {
        return *this;
    }
    void mark_deleted_tree() {
        v_ |= P::deleted_bit;
    }
    void mark_root() {
        v_ |= P::root_bit;
    }
    void mark_nonroot() {
        v_ &= ~P::root_bit;
    }

    void assign_version(singlethreaded_nodeversion<P> x) {
        v_ = x.v_;
    }

    value_type version_value() const {
        return v_;
    }
    value_type unlocked_version_value() const {
        return v_;
    }

  private:
    value_type v_;
};


template <typename V> struct nodeversion_parameters {};

template <> struct nodeversion_parameters<uint32_t> {
    enum {
        lock_bit = (1U << 0),
        inserting_shift = 1,
        inserting_bit = (1U << 1),
        splitting_bit = (1U << 2),
        dirty_mask = inserting_bit | splitting_bit,
        vinsert_lowbit = (1U << 3), // == inserting_bit << 2
        vsplit_lowbit = (1U << 9),
        // unused1_bit = (1U << 28),
        migration_bit = (1U << 28),
        deleted_bit = (1U << 29),
        root_bit = (1U << 30),
        isleaf_bit = (1U << 31),
        split_unlock_mask = ~(root_bit | migration_bit | (vsplit_lowbit - 1)),
        unlock_mask = ~(migration_bit | (vinsert_lowbit - 1)),
        top_stable_bits = 4
    };

    typedef uint32_t value_type;
};

template <> struct nodeversion_parameters<uint64_t> {
    enum {
        lock_bit = (1ULL << 8),
        inserting_shift = 9,
        inserting_bit = (1ULL << 9),
        splitting_bit = (1ULL << 10),
        dirty_mask = inserting_bit | splitting_bit,
        vinsert_lowbit = (1ULL << 11), // == inserting_bit << 2
        vsplit_lowbit = (1ULL << 27),
        unused1_bit = (1ULL << 60),
        deleted_bit = (1ULL << 61),
        root_bit = (1ULL << 62),
        isleaf_bit = (1ULL << 63),
        split_unlock_mask = ~(root_bit | unused1_bit | (vsplit_lowbit - 1)),
        unlock_mask = ~(unused1_bit | (vinsert_lowbit - 1)),
        top_stable_bits = 4
    };

    typedef uint64_t value_type;
};

typedef nodeversion<nodeversion_parameters<uint32_t> > nodeversion32;

#endif
