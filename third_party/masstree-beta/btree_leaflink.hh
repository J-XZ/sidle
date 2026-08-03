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
#ifndef BTREE_LEAFLINK_HH
#define BTREE_LEAFLINK_HH 1
#include "compiler.hh"
#include "dsidle/swcc_visibility.h"

/** @brief Operations to manage linked lists of B+tree leaves.

    N is the type of nodes. CONCURRENT is true to make operations
    concurrency-safe (e.g. compare-and-swaps, fences), false to leave them
    unsafe (only OK on single threaded code, but faster). */
template <typename N, bool CONCURRENT = N::concurrent> struct btree_leaflink {};


// This is the normal version of btree_leaflink; it uses lock-free linked list
// operations.
template <typename N> struct btree_leaflink<N, true> {
  private:
    static inline N* invalidate_next(N* n) {
        dsidle::InvalidateSwccRange(&n->next_, sizeof(n->next_));
        return latency_sim::FixedLatencyMemoryLoadAs<N*>(
            latency_sim::PoolKind::kSwcc, &n->next_);
    }
    static inline N* invalidate_prev(N* n) {
        dsidle::InvalidateSwccRange(&n->prev_, sizeof(n->prev_));
        return latency_sim::FixedLatencyMemoryLoadAs<N*>(
            latency_sim::PoolKind::kSwcc, &n->prev_);
    }
    static inline void flush_next(N* n) {
        dsidle::FlushSwccRange(&n->next_, sizeof(n->next_));
    }
    static inline void flush_prev(N* n) {
        dsidle::FlushSwccRange(&n->prev_, sizeof(n->prev_));
    }
    template <typename SF>
    static inline void lock_link(N* n, SF spin_function) {
        while (!dsidle::TryLockLeafLink(n->control_ref()))
            spin_function();
    }
    static inline void unlock_link(N* n) {
        dsidle::UnlockLeafLink(n->control_ref());
    }
    template <typename SF>
    static inline N *lock_next(N *n, SF spin_function) {
        lock_link(n, spin_function);
        return invalidate_next(n);
    }

  public:
    /** @brief Insert a new node @a nr at the right of node @a n.
        @pre @a n is locked.

        Concurrency correctness: Ensures that all "next" pointers are always
        valid, even if @a n's successor is deleted concurrently. */
    static void link_split(N *n, N *nr) {
        link_split(n, nr, relax_fence_function());
    }
    /** @overload */
    template <typename SF>
    static void link_split(N *n, N *nr, SF spin_function) {
        latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                             &nr->prev_, n);
        N *next = lock_next(n, spin_function);
        latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                             &nr->next_, next);
        // nr is not reachable yet. Publish its complete initialized body,
        // including both links, before the predecessor edge exposes it.
        nr->publish_body_before_edge();
        if (next) {
            latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                                 &next->prev_, nr);
            flush_prev(next);
        }
        fence();
        latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                             &n->next_, nr);
        flush_next(n);
        unlock_link(n);
    }

    /** @brief Change the prev link of @a n's next node to @a nn.
    @pre @a n is locked, nn is locked

    Concurrency correctness: Ensures that all "next" pointers are always
    valid, even if @a n's successor is deleted concurrently. */
    static void change_link(N *n, N *nn) {
        change_link(n, nn, relax_fence_function());
    }

    template <typename SF>
    static void change_link(N *n, N *nn, SF spin_function) {
        N *next = lock_next(n, spin_function);
        latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                             &nn->next_, next);
        flush_next(nn);
        if (next) {
            latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                                 &next->prev_, nn);
            flush_prev(next);
        }
        fence();
        N *prev = nullptr;
        while (1) {
            prev = invalidate_prev(n);
            if (!prev)
                break;
            lock_link(prev, spin_function);
            const N* observed_next = invalidate_next(prev);
            if (observed_next && observed_next->control_ref() ==
                    n->control_ref())
                break;
            unlock_link(prev);
            spin_function();
        }
        if (prev) {
            latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                                 &prev->next_, nn);
            latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                                 &nn->prev_, prev);
            flush_next(prev);
            flush_prev(nn);
            unlock_link(prev);
        }
        unlock_link(n);
    }

    /** @brief Unlink @a n from the list.
        @pre @a n is locked.

        Concurrency correctness: Works even in the presence of concurrent
        splits and deletes. */
    static void unlink(N *n) {
        unlink(n, relax_fence_function());
    }
    /** @overload */
    template <typename SF>
    static void unlink(N *n, SF spin_function) {
        // Assume node order A <-> N <-> B. Since n is locked, n cannot split;
        // next node will always be B or one of its successors.
        N *next = lock_next(n, spin_function);
        N *prev;
        while (1) {
            prev = invalidate_prev(n);
            lock_link(prev, spin_function);
            const N* observed_next = invalidate_next(prev);
            if (observed_next && observed_next->control_ref() ==
                    n->control_ref())
                break;
            unlock_link(prev);
            spin_function();
        }
        if (next) {
            latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                                 &next->prev_, prev);
            flush_prev(next);
        }
        fence();
        latency_sim::FixedLatencyMemoryStoreValue(latency_sim::PoolKind::kSwcc,
                                             &prev->next_, next);
        flush_next(prev);
        unlock_link(prev);
        unlock_link(n);
    }
};


// This is the single-threaded-only fast version of btree_leaflink.
template <typename N> struct btree_leaflink<N, false> {
    static void link_split(N *n, N *nr) {
        link_split(n, nr, do_nothing());
    }
    template <typename SF>
    static void link_split(N *n, N *nr, SF) {
        nr->prev_ = n;
        nr->next_ = static_cast<N*>(n->next_);
        n->next_ = nr;
        if (N* next = nr->next_)
            next->prev_ = nr;
    }
    static void unlink(N *n) {
        unlink(n, do_nothing());
    }
    template <typename SF>
    static void unlink(N *n, SF) {
        if (N* next = n->next_)
            next->prev_ = static_cast<N*>(n->prev_);
        static_cast<N*>(n->prev_)->next_ = static_cast<N*>(n->next_);
    }
};

#endif
