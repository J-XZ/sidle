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
#include "kvthread.hh"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <sys/mman.h>
#if HAVE_SUPERPAGE && !NOSUPERPAGE
#include <sys/types.h>
#include <dirent.h>
#endif

threadinfo *threadinfo::allthreads;
// dsidle: process-local compatibility for the pre-M4 RCU implementation.
// M4 replaces these with the HWCC epoch table.
volatile mrcu_epoch_type globalepoch = 1;
volatile mrcu_epoch_type active_epoch = 1;
#if ENABLE_ASSERTIONS
int threadinfo::no_pool_value;
#endif

inline threadinfo::threadinfo(int purpose, int index) {
    gc_epoch_ = perform_gc_epoch_ = 0;
    logger_ = nullptr;
    next_ = nullptr;
    purpose_ = purpose;
    index_ = index;
    rcu_operations_ = 0;

    for (size_t i = 0; i != sizeof(pool_) / sizeof(pool_[0]); ++i) {
        pool_[i] = nullptr;
    }

    // initialize the remote pool
    for (size_t i = 0; i != sizeof(remote_pool_) / sizeof(remote_pool_[0]); ++i) {
        remote_pool_[i] = nullptr;
    }

    void *limbo_space = allocate(sizeof(limbo_group), memtag_limbo);
    mark(tc_limbo_slots, limbo_group::capacity);
    limbo_head_ = limbo_tail_ = new(limbo_space) limbo_group;
    ts_ = 2;

    for (size_t i = 0; i != sizeof(counters_) / sizeof(counters_[0]); ++i) {
        counters_[i] = 0;
    }
}

threadinfo *threadinfo::make(int purpose, int index) {
    static int threads_initialized;
    #ifdef CXL
    threadinfo* ti = new(malloc_with_cxl(8192)) threadinfo(purpose, index);
    #else
    threadinfo* ti = new(malloc(8192)) threadinfo(purpose, index);
    #endif
    ti->next_ = allthreads;
    allthreads = ti;

    if (!threads_initialized) {
#if ENABLE_ASSERTIONS
        const char* s = getenv("_");
        no_pool_value = s && strstr(s, "valgrind") != 0;
#endif
        threads_initialized = 1;
    }

    return ti;
}

void threadinfo::refill_rcu() {
    if (!limbo_tail_->next_) {
        void *limbo_space = allocate(sizeof(limbo_group), memtag_limbo);
        mark(tc_limbo_slots, limbo_group::capacity);
        limbo_tail_->next_ = new(limbo_space) limbo_group;
    }
    limbo_tail_ = limbo_tail_->next_;
    always_assert(
        limbo_tail_->head_ == 0 && limbo_tail_->tail_ == 0,
        "reused RCU limbo group is not empty");
}

inline unsigned limbo_group::clean_until(threadinfo& ti, mrcu_epoch_type epoch_bound,
                                         unsigned count) {
    epoch_type epoch = 0;
    while (head_ != tail_) {
        if (e_[head_].ref_) {
            ti.free_rcu(e_[head_].ref_, e_[head_].u_.allocation.tag,
                        e_[head_].u_.allocation.owner_shard, e_[head_].node_ref_);
            ti.mark(tc_gc);
            --count;
            if (!count) {
                e_[head_].ref_ = 0;
                e_[head_].node_ref_ = dsidle::NodeRef();
                e_[head_].u_.epoch = epoch;
                break;
            }
        } else {
            epoch = e_[head_].u_.epoch;
            if (signed_epoch_type(epoch_bound - epoch) < 0)
                break;
        }
        ++head_;
    }
    if (head_ == tail_)
        head_ = tail_ = 0;
    return count;
}

void threadinfo::hard_rcu_quiesce() {
    limbo_group* empty_head = nullptr;
    limbo_group* empty_tail = nullptr;
    unsigned count = rcu_free_count;

    // dsidle: do not reuse an object until every VM/thread slot has moved
    // beyond its retirement epoch.
    const auto minimum = dsidle::SharedEpochSlots(dsidle::CurrentSharedPool()).MinimumActive();
    const auto current = dsidle::SharedEpochState(dsidle::CurrentSharedPool()).Current();
    mrcu_epoch_type epoch_bound = (minimum == dsidle::kEpochInactive ? current : minimum) - 1;
    if (limbo_head_->head_ == limbo_head_->tail_
        || mrcu_signed_epoch_type(epoch_bound - limbo_head_->first_epoch()) < 0)
        goto done;

    // clean [limbo_head_, limbo_tail_]
    while (count) {
        count = limbo_head_->clean_until(*this, epoch_bound, count);
        if (limbo_head_->head_ != limbo_head_->tail_)
            break;
        if (!empty_head)
            empty_head = limbo_head_;
        empty_tail = limbo_head_;
        if (limbo_head_ == limbo_tail_) {
            limbo_head_ = limbo_tail_ = empty_head;
            goto done;
        }
        limbo_head_ = limbo_head_->next_;
    }
    // hook empties after limbo_tail_
    if (empty_head) {
        empty_tail->next_ = limbo_tail_->next_;
        limbo_tail_->next_ = empty_head;
    }

done:
    if (!count)
        perform_gc_epoch_ = epoch_bound; // do GC again immediately
    else
        perform_gc_epoch_ = epoch_bound + 1;
}

void threadinfo::report_rcu(void *ptr) const
{
    for (limbo_group *lg = limbo_head_; lg; lg = lg->next_) {
        int status = 0;
        limbo_group::epoch_type e = 0;
        for (unsigned i = 0; i < lg->capacity; ++i) {
            if (i == lg->head_)
                status = 1;
            if (i == lg->tail_) {
                status = 0;
                e = 0;
            }
            const uint64_t ref = (lg->e_[i].u_.allocation.tag != memtag(-1) &&
                                  (lg->e_[i].u_.allocation.tag & memtag_pool_mask))
                ? uint64_t(reinterpret_cast<std::byte*>(ptr) - static_cast<std::byte*>(dsidle::SharedPoolBase()))
                : reinterpret_cast<uint64_t>(ptr);
            if (lg->e_[i].ref_ == ref)
                fprintf(stderr, "thread %d: rcu %p@%d: %s as %x @%" PRIu64 "\n",
                        index_, lg, i, status ? "waiting" : "freed",
                        lg->e_[i].u_.allocation.tag, e);
            else if (!lg->e_[i].ref_)
                e = lg->e_[i].u_.epoch;
        }
    }
}

void threadinfo::report_rcu_all(void *ptr)
{
    for (threadinfo *ti = allthreads; ti; ti = ti->next())
        ti->report_rcu(ptr);
}


#if HAVE_SUPERPAGE && !NOSUPERPAGE
static size_t read_superpage_size() {
    if (DIR* d = opendir("/sys/kernel/mm/hugepages")) {
        size_t n = (size_t) -1;
        while (struct dirent* de = readdir(d))
            if (de->d_type == DT_DIR
                && strncmp(de->d_name, "hugepages-", 10) == 0
                && de->d_name[10] >= '0' && de->d_name[10] <= '9') {
                size_t x = strtol(&de->d_name[10], 0, 10) << 10;
                n = (x < n ? x : n);
            }
        closedir(d);
        return n;
    } else
        return 2 << 20;
}

static size_t superpage_size = 0;
#endif

static void initialize_pool(void* pool, size_t sz, size_t unit) {
    char* p = reinterpret_cast<char*>(pool);
    void** nextptr = reinterpret_cast<void**>(p);
    for (size_t off = unit; off + unit <= sz; off += unit) {
        *nextptr = p + off;
        nextptr = reinterpret_cast<void**>(p + off);
    }
    *nextptr = 0;
}

void threadinfo::refill_pool(int nl) {
    assert(!pool_[nl - 1]);

    if (!use_pool()) {
        #ifdef CXL
        pool_[nl - 1] = malloc_with_cxl(nl * CACHE_LINE_SIZE);
        #else 
        pool_[nl - 1] = malloc(nl * CACHE_LINE_SIZE);
        #endif
        if (pool_[nl - 1])
            *reinterpret_cast<void**>(pool_[nl - 1]) = 0;
        return;
    }

    void* pool = 0;
    size_t pool_size = 0;
    int r;

#if HAVE_SUPERPAGE && !NOSUPERPAGE
    if (!superpage_size)
        superpage_size = read_superpage_size();
    if (superpage_size != (size_t) -1) {
        pool_size = superpage_size;
# if MADV_HUGEPAGE
        if ((r = posix_memalign(&pool, pool_size, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign superpage: %s\n", strerror(r));
            pool = 0;
            superpage_size = (size_t) -1;
        } else if (madvise(pool, pool_size, MADV_HUGEPAGE) != 0) {
            perror("madvise superpage");
            superpage_size = (size_t) -1;
        }
# elif MAP_HUGETLB
        pool = mmap(0, pool_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (pool == MAP_FAILED) {
            perror("mmap superpage");
            pool = 0;
            superpage_size = (size_t) -1;
        }
# else
        superpage_size = (size_t) -1;
# endif
    }
#endif

    if (!pool) {
        pool_size = 2 << 20;
        #ifdef CXL
        if ((r = posix_memalign_with_cxl(&pool, CACHE_LINE_SIZE, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(r));
            abort();
        } 
        #else
        if ((r = posix_memalign(&pool, CACHE_LINE_SIZE, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(r));
            abort();
        }
        #endif
    }

    initialize_pool(pool, pool_size, nl * CACHE_LINE_SIZE);
    pool_[nl - 1] = pool;
}

void threadinfo::refill_remote_pool(int nl) {
    assert(!remote_pool_[nl - 1]);

    if (!use_pool()) {
        remote_pool_[nl - 1] = malloc(nl * CACHE_LINE_SIZE);
        if (remote_pool_[nl - 1])
            *reinterpret_cast<void**>(remote_pool_[nl - 1]) = 0;
    }

    void *remote_pool = 0;
    size_t pool_size = 0;
    int r;

    if (!remote_pool) {
        pool_size = 2 << 20;
        if ((r = posix_memalign(&remote_pool, CACHE_LINE_SIZE, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(r));
            abort();
        }
    }

    initialize_pool(remote_pool, pool_size, nl * CACHE_LINE_SIZE);
    remote_pool_[nl - 1] = remote_pool;
}

void threadinfo::refill_local_pool(int nl) {
    assert(!pool_[nl - 1]);
    if (!use_pool()) {
        pool_[nl - 1] = malloc(nl * CACHE_LINE_SIZE);
        if (pool_[nl - 1])
            *reinterpret_cast<void**>(pool_[nl - 1]) = 0;
        return;
    }
    void* pool = 0;
    size_t pool_size = 0;
    int r;
    
    if (!pool) {
        pool_size = 2 << 20;
        if ((r = posix_memalign(&pool, CACHE_LINE_SIZE, pool_size)) != 0) {
            fprintf(stderr, "posix_memalign: %s\n", strerror(r));
            abort();
        }
    }

    initialize_pool(pool, pool_size, nl * CACHE_LINE_SIZE);
    pool_[nl - 1] = pool;
}
