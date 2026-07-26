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
#ifndef KVTHREAD_HH
#define KVTHREAD_HH 1
#include "cxl_cpp_allocator.hh"
#include "dsidle/shared_pool.h"
#include "mtcounters.hh"
#include "compiler.hh"
#include "circular_int.hh"
#include "timestamp.hh"
#include "memdebug.hh"
#include <assert.h>
#include <pthread.h>
#include <chrono>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

// #define TRANSPARENT_CXL
// #define THREAD_DEBUG

class threadinfo;
class loginfo;

typedef uint64_t mrcu_epoch_type;
typedef int64_t mrcu_signed_epoch_type;

extern volatile mrcu_epoch_type globalepoch;  // global epoch, updated regularly
extern volatile mrcu_epoch_type active_epoch;

struct limbo_group {
    typedef mrcu_epoch_type epoch_type;
    typedef mrcu_signed_epoch_type signed_epoch_type;

    struct limbo_element {
        // dsidle: canonical pooled objects are recorded as SWCC offsets.
        // Non-pooled callback objects remain process-local addresses.
        uint64_t ref_;
        // A retired tree node keeps its HWCC control reference until its SWCC
        // body is actually returned to the owner shard.
        dsidle::NodeRef node_ref_;
        union {
            struct {
                memtag tag;
                std::uint32_t owner_shard;
            } allocation;
            epoch_type epoch;
        } u_;
    };

    enum { capacity = (4076 - sizeof(epoch_type) - sizeof(limbo_group*)) / sizeof(limbo_element) };
    unsigned head_;
    unsigned tail_;
    epoch_type epoch_;
    limbo_group* next_;
    limbo_element e_[capacity];
    limbo_group()
        : head_(0), tail_(0), next_() {
    }
    epoch_type first_epoch() const {
        assert(head_ != tail_);
        return e_[head_].u_.epoch;
    }
    void push_back(void* ptr, memtag tag, std::uint32_t owner_shard,
                   dsidle::NodeRef node_ref, mrcu_epoch_type epoch) {
        assert(tail_ + 2 <= capacity);
        if (head_ == tail_ || epoch_ != epoch) {
            e_[tail_].ref_ = 0;
            e_[tail_].node_ref_ = dsidle::NodeRef();
            e_[tail_].u_.epoch = epoch;
            epoch_ = epoch;
            ++tail_;
        }
        e_[tail_].ref_ = (tag != memtag(-1) && (tag & memtag_pool_mask))
            ? uint64_t(reinterpret_cast<std::byte*>(ptr) - static_cast<std::byte*>(dsidle::SharedPoolBase()))
            : reinterpret_cast<uint64_t>(ptr);
        e_[tail_].node_ref_ = node_ref;
        e_[tail_].u_.allocation = {tag, owner_shard};
        ++tail_;
    }
    inline unsigned clean_until(threadinfo& ti, mrcu_epoch_type epoch_bound, unsigned count);
};

template <int N> struct has_threadcounter {
    static bool test(threadcounter ci) {
        return unsigned(ci) < unsigned(N);
    }
};
template <> struct has_threadcounter<0> {
    static bool test(threadcounter) {
        return false;
    }
};

struct mrcu_callback {
    virtual ~mrcu_callback() {
    }
    virtual void operator()(threadinfo& ti) = 0;
};

class threadinfo {
  public:
    enum {
        TI_MAIN, TI_PROCESS, TI_LOG, TI_CHECKPOINT, TI_MIGRATION
    };

    static threadinfo* allthreads;

    threadinfo* next() const {
        return next_;
    }

    static threadinfo* make(int purpose, int index);
    // XXX destructor

    // thread information
    int purpose() const {
        return purpose_;
    }
    int index() const {
        return index_;
    }
    loginfo* logger() const {
        return logger_;
    }
    void set_logger(loginfo* logger) {
        assert(!logger_ && logger);
        logger_ = logger;
    }

    // timestamps
    kvtimestamp_t operation_timestamp() const {
        return timestamp();
    }
    kvtimestamp_t update_timestamp() const {
        return ts_;
    }
    kvtimestamp_t update_timestamp(kvtimestamp_t x) const {
        if (circular_int<kvtimestamp_t>::less_equal(ts_, x))
            // x might be a marker timestamp; ensure result is not
            ts_ = (x | 1) + 1;
        return ts_;
    }
    template <typename N> void observe_phantoms(N* n) {
        const auto phantom_epoch = n->phantom_epoch();
        if (circular_int<kvtimestamp_t>::less(ts_, phantom_epoch))
            ts_ = phantom_epoch;
    }

    // event counters
    void mark(threadcounter ci) {
        if (has_threadcounter<int(ncounters)>::test(ci))
            ++counters_[ci];
    }
    void mark(threadcounter ci, int64_t delta) {
        if (has_threadcounter<int(ncounters)>::test(ci))
            counters_[ci] += delta;
    }
    void set_counter(threadcounter ci, uint64_t value) {
        if (has_threadcounter<int(ncounters)>::test(ci))
            counters_[ci] = value;
    }
    bool has_counter(threadcounter ci) const {
        return has_threadcounter<int(ncounters)>::test(ci);
    }
    uint64_t counter(threadcounter ci) const {
        return has_threadcounter<int(ncounters)>::test(ci) ? counters_[ci] : 0;
    }

    struct accounting_relax_fence_function {
        threadinfo* ti_;
        threadcounter ci_;
        accounting_relax_fence_function(threadinfo* ti, threadcounter ci)
            : ti_(ti), ci_(ci) {
        }
        void operator()() {
            relax_fence();
            ti_->mark(ci_);
        }
    };
    /** @brief Return a function object that calls mark(ci); relax_fence().
     *
     * This function object can be used to count the number of relax_fence()s
     * executed. */
    accounting_relax_fence_function accounting_relax_fence(threadcounter ci) {
        return accounting_relax_fence_function(this, ci);
    }

    struct stable_accounting_relax_fence_function {
        threadinfo* ti_;
        stable_accounting_relax_fence_function(threadinfo* ti)
            : ti_(ti) {
        }
        template <typename V>
        void operator()(V v) {
            relax_fence();
            ti_->mark(threadcounter(tc_stable + (v.isleaf() << 1) + v.splitting()));
        }
    };
    /** @brief Return a function object that calls mark(ci); relax_fence().
     *
     * This function object can be used to count the number of relax_fence()s
     * executed. */
    stable_accounting_relax_fence_function stable_fence() {
        return stable_accounting_relax_fence_function(this);
    }

    accounting_relax_fence_function lock_fence(threadcounter ci) {
        return accounting_relax_fence_function(this, ci);
    }

    // memory allocation
    void* allocate(size_t sz, memtag tag) {
        if (tag == memtag_value || tag == memtag_masstree_ksuffixes)
            return pool_allocate(sz, tag);
        void* p = malloc(sz + memdebug_size);
        p = memdebug::make(p, sz, tag);
        if (p)
            mark(threadcounter(tc_alloc + (tag > memtag_value)), sz);
        return p;
    }
    void deallocate(void* p, size_t sz, memtag tag) {
        // in C++ allocators, 'p' must be nonnull
        assert(p);
        if (tag == memtag_value || tag == memtag_masstree_ksuffixes) {
            pool_deallocate(p, sz, tag);
            return;
        }
        p = memdebug::check_free(p, sz, tag);
        free(p);
        mark(threadcounter(tc_alloc + (tag > memtag_value)), -sz);
    }
    void deallocate_rcu(void* p, size_t sz, memtag tag) {
        assert(p);
        if (tag == memtag_value || tag == memtag_masstree_ksuffixes) {
            pool_deallocate_rcu(p, sz, tag);
            return;
        }
        memdebug::check_rcu(p, sz, tag);
        record_rcu(p, tag);
        mark(threadcounter(tc_alloc + (tag > memtag_value)), -sz);
    }

#ifdef THREAD_DEBUG
    void* pool_allocate(size_t sz, memtag tag, bool is_migration = false) {
#else
    void* pool_allocate(size_t sz, memtag tag) {
#endif
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(nl <= pool_max_nlines);
        // dsidle: canonical nodes, values, and suffixes always use SWCC.
        void* p = dsidle::AllocateCurrentSwcc(nl * CACHE_LINE_SIZE).get(dsidle::SharedPoolBase());
        p = memdebug::make(p, sz, memtag(tag + nl));
        mark(threadcounter(tc_alloc + (tag > memtag_value)), nl * CACHE_LINE_SIZE);
        return p;
    }

    void pool_deallocate(void* p, size_t sz, memtag tag) {
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(p && nl <= pool_max_nlines);
        p = memdebug::check_free(p, sz, memtag(tag + nl));
        dsidle::FreeCurrentSwcc(dsidle::SwccOffset<std::byte>(
            reinterpret_cast<std::byte*>(p) - static_cast<std::byte*>(dsidle::SharedPoolBase())),
            nl * CACHE_LINE_SIZE);
        mark(threadcounter(tc_alloc + (tag > memtag_value)),
             -nl * CACHE_LINE_SIZE);
    }
    void pool_deallocate_rcu(void* p, size_t sz, memtag tag, dsidle::NodeRef node_ref = {}) {
        int nl = (sz + memdebug_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
        assert(p && nl <= pool_max_nlines);
        memdebug::check_rcu(p, sz, memtag(tag + nl));
        if (node_ref) {
            const auto epoch = gc_epoch_ ? gc_epoch_
                : dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Current();
            dsidle::NodeControlSlab(dsidle::CurrentSharedPool()).Retire(node_ref, epoch);
        }
        record_rcu(p, memtag(tag + nl), node_ref);
        mark(threadcounter(tc_alloc + (tag > memtag_value)),
             -nl * CACHE_LINE_SIZE);
    }

    // RCU
    enum { rcu_free_count = 128 }; // max # of entries to free per rcu_quiesce() call
    class rcu_scope {
      public:
        explicit rcu_scope(threadinfo& ti) : ti_(ti) { ti_.rcu_start(); }
        ~rcu_scope() { ti_.rcu_stop(); }
      private:
        threadinfo& ti_;
    };
    void rcu_start() {
        // dsidle: publish foreground participation in the HWCC slot assigned
        // to this VM/shard and threadinfo index.
        const mrcu_epoch_type epoch = dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Current();
        if (gc_epoch_ != epoch)
            gc_epoch_ = epoch;
        dsidle::SharedEpochSlots(dsidle::CurrentSharedPool()).Enter(
            dsidle::CurrentSwccShard(), static_cast<std::uint32_t>(index_), gc_epoch_);
    }
    void rcu_stop() {
        dsidle::SharedEpochSlots(dsidle::CurrentSharedPool()).Leave(
            dsidle::CurrentSwccShard(), static_cast<std::uint32_t>(index_));
        gc_epoch_ = 0;
        // dsidle: keep the global advance/minimum scan out of every API's
        // hot path; this matches the baseline's 50-operation cadence.
        if (++rcu_operations_ == 50) {
            rcu_operations_ = 0;
            rcu_quiesce();
        }
    }
    void rcu_quiesce() {    
        dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Advance();
        if (perform_gc_epoch_ != dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Current())
            hard_rcu_quiesce();
    }
    // dsidle: callers invoke this before joining a worker. Limbo remains
    // owned by its creating thread until every deferred object is reclaimed.
    void rcu_drain() {
        if (gc_epoch_) {
            dsidle::SharedEpochSlots(dsidle::CurrentSharedPool()).Leave(
                dsidle::CurrentSwccShard(), static_cast<std::uint32_t>(index_));
            gc_epoch_ = 0;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!limbo_empty()) {
            rcu_quiesce();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("D-SIDLE RCU limbo did not drain before thread exit");
        }
        dsidle::SharedEpochSlots(dsidle::CurrentSharedPool()).Leave(
            dsidle::CurrentSwccShard(), static_cast<std::uint32_t>(index_));
    }
    typedef ::mrcu_callback mrcu_callback;
    void rcu_register(mrcu_callback* cb) {
        record_rcu(cb, memtag(-1));
    }

    // thread management
    pthread_t& pthread() {
        return pthreadid_;
    }
    pthread_t pthread() const {
        return pthreadid_;
    }

    void report_rcu(void* ptr) const;
    static void report_rcu_all(void* ptr);
    static inline mrcu_epoch_type min_active_epoch();

  private:
    union {
        struct {
            mrcu_epoch_type gc_epoch_;
            mrcu_epoch_type perform_gc_epoch_;
            loginfo *logger_;

            threadinfo *next_;
            int purpose_;
            int index_;         // the index of a udp, logging, tcp,
                                // checkpoint or recover thread

            pthread_t pthreadid_;
            std::uint32_t rcu_operations_;
        };
        char padding1[CACHE_LINE_SIZE];
    };

    enum { pool_max_nlines = 20 };
    void* pool_[pool_max_nlines];
    void* remote_pool_[pool_max_nlines];

    limbo_group* limbo_head_;
    limbo_group* limbo_tail_;
    mutable kvtimestamp_t ts_;

    //enum { ncounters = (int) tc_max };
    enum { ncounters = 0 };
    uint64_t counters_[ncounters];

    void refill_pool(int nl);
    void refill_remote_pool(int nl);
    void refill_local_pool(int nl);
    void refill_rcu();
    bool limbo_empty() const {
        return limbo_head_ == limbo_tail_ && limbo_head_->head_ == limbo_head_->tail_;
    }

    void free_rcu(uint64_t ref, memtag tag, std::uint32_t owner_shard, dsidle::NodeRef node_ref) {
        void* p = (tag != memtag(-1) && (tag & memtag_pool_mask))
            ? dsidle::SwccOffset<std::byte>(ref).get(dsidle::SharedPoolBase())
            : reinterpret_cast<void*>(ref);
        if ((tag & memtag_pool_mask) == 0) {
            p = memdebug::check_free_after_rcu(p, tag);
            ::free(p);
        } else if (tag == memtag(-1))
            (*static_cast<mrcu_callback*>(p))(*this);
        else {
            p = memdebug::check_free_after_rcu(p, tag);
            int nl = tag & memtag_pool_mask;
            dsidle::FreeCurrentSwccToOwner(owner_shard, dsidle::SwccOffset<std::byte>(
                reinterpret_cast<std::byte*>(p) - static_cast<std::byte*>(dsidle::SharedPoolBase())),
                nl * CACHE_LINE_SIZE);
            if (node_ref)
                dsidle::NodeControlSlab(dsidle::CurrentSharedPool()).Release(node_ref);
        }
    }

    void record_rcu(void* ptr, memtag tag, dsidle::NodeRef node_ref = {}) {
        if (limbo_tail_->tail_ + 2 > limbo_tail_->capacity)
            refill_rcu();
        const uint64_t epoch = gc_epoch_ ? gc_epoch_
            : dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Current();
        const std::uint32_t owner_shard = (tag != memtag(-1) && (tag & memtag_pool_mask))
            ? dsidle::CurrentSwccOwner(dsidle::SwccOffset<std::byte>(
                  reinterpret_cast<std::byte*>(ptr) - static_cast<std::byte*>(dsidle::SharedPoolBase())),
                  (tag & memtag_pool_mask) * CACHE_LINE_SIZE)
            : 0;
        limbo_tail_->push_back(ptr, tag, owner_shard, node_ref, epoch);
    }

#if ENABLE_ASSERTIONS
    static int no_pool_value;
#endif
    static bool use_pool() {
#if ENABLE_ASSERTIONS
        return !no_pool_value;
#else
        return true;
#endif
    }

    inline threadinfo(int purpose, int index);
    threadinfo(const threadinfo&) = delete;
    ~threadinfo() {}
    threadinfo& operator=(const threadinfo&) = delete;

    void hard_rcu_quiesce();

    friend struct limbo_group;
};

inline mrcu_epoch_type threadinfo::min_active_epoch() {
    mrcu_epoch_type ae = globalepoch;
    for (threadinfo* ti = allthreads; ti; ti = ti->next()) {
        prefetch((const void*) ti->next());
        mrcu_epoch_type te = ti->gc_epoch_;
        if (te && mrcu_signed_epoch_type(te - ae) < 0)
            ae = te;
    }
    return ae;
}

#endif
