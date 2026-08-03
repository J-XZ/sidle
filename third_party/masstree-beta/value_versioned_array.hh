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
#ifndef VALUE_VERSIONED_ARRAY_HH
#define VALUE_VERSIONED_ARRAY_HH
#include "compiler.hh"
#include "value_array.hh"

struct rowversion {
    rowversion() {
        v_.u = 0;
    }
    bool dirty() {
        return dsidle_masstree::LoadSwcc(&v_).dirty;
    }
    void setdirty() {
        value_t value = dsidle_masstree::LoadSwcc(&v_);
        value.u |= 0x80000000;
        dsidle_masstree::StoreSwcc(&v_, value);
    }
    void clear() {
        value_t value = dsidle_masstree::LoadSwcc(&v_);
        value.u &= 0x7fffffff;
        dsidle_masstree::StoreSwcc(&v_, value);
    }
    void clearandbump() {
        value_t value = dsidle_masstree::LoadSwcc(&v_);
        value.u = (value.u + 1) & 0x7fffffff;
        dsidle_masstree::StoreSwcc(&v_, value);
    }
    rowversion stable() const {
        value_t x = dsidle_masstree::LoadSwcc(&v_);
        while (x.dirty) {
            relax_fence();
            x = dsidle_masstree::LoadSwcc(&v_);
        }
        acquire_fence();
        return x;
    }
    bool has_changed(rowversion x) const {
        fence();
        return x.v_.ctr != dsidle_masstree::LoadSwcc(&v_).ctr;
    }
  private:
    union value_t {
        struct {
            uint32_t ctr:31;
            uint32_t dirty:1;
        };
        uint32_t u;
    };
    value_t v_;

    rowversion(value_t v)
        : v_(v) {
    }

};

class value_versioned_array {
  public:
    typedef value_array::index_type index_type;
    static const char *name() { return "ArrayVersion"; }

    typedef lcdf::Json Json;

    inline value_versioned_array();

    inline kvtimestamp_t timestamp() const;
    inline int ncol() const;
    inline Str col(int i) const;

    void deallocate(threadinfo &ti);
    void deallocate_rcu(threadinfo &ti);

    void snapshot(value_versioned_array*& storage,
                  const std::vector<index_type>& f, threadinfo& ti) const;

    value_versioned_array* update(const Json* first, const Json* last,
                                  kvtimestamp_t ts, threadinfo& ti,
                                  bool always_copy = false);
    static value_versioned_array* create(const Json* first, const Json* last,
                                         kvtimestamp_t ts, threadinfo& ti);
    static value_versioned_array* create1(Str value, kvtimestamp_t ts, threadinfo& ti);
    inline void deallocate_rcu_after_update(const Json* first, const Json* last, threadinfo& ti);
    inline void deallocate_after_failed_update(const Json* first, const Json* last, threadinfo& ti);

    template <typename PARSER>
    static value_versioned_array* checkpoint_read(PARSER& par, kvtimestamp_t ts,
                                                  threadinfo& ti);
    template <typename UNPARSER>
    void checkpoint_write(UNPARSER& unpar) const;

    void print(FILE *f, const char *prefix, int indent, Str key,
               kvtimestamp_t initial_ts, const char *suffix = "") {
        kvtimestamp_t adj_ts = timestamp_sub(timestamp(), initial_ts);
        fprintf(f, "%s%*s%.*s = ### @" PRIKVTSPARTS "%s\n", prefix, indent, "",
                key.len, key.s, KVTS_HIGHPART(adj_ts), KVTS_LOWPART(adj_ts), suffix);
    }

  private:
    kvtimestamp_t ts_;
    rowversion ver_;
    short ncol_;
    short ncol_cap_;
    lcdf::inline_string* cols_[0];

    static inline size_t shallow_size(int ncol);
    inline size_t shallow_size() const;
    static value_versioned_array* make_sized_row(int ncol, kvtimestamp_t ts, threadinfo& ti);
};

template <>
struct query_helper<value_versioned_array> {
    value_versioned_array* snapshot_;

    query_helper()
        : snapshot_() {
    }
    inline const value_versioned_array* snapshot(const value_versioned_array* row,
                                                 const std::vector<value_versioned_array::index_type>& f,
                                                 threadinfo& ti) {
        row->snapshot(snapshot_, f, ti);
        return snapshot_;
    }
};

inline value_versioned_array::value_versioned_array()
    : ts_(0), ncol_(0), ncol_cap_(0) {
}

inline kvtimestamp_t value_versioned_array::timestamp() const {
    return dsidle_masstree::LoadSwcc(&ts_);
}

inline int value_versioned_array::ncol() const {
    return dsidle_masstree::LoadSwcc(&ncol_);
}

inline Str value_versioned_array::col(int i) const {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    if (unsigned(i) >= unsigned(ncol))
        return Str();
    auto* col = dsidle_masstree::LoadSwcc(&cols_[i]);
    if (!col)
        return Str();
    const auto len = dsidle_masstree::LoadSwcc(&col->len);
    dsidle_masstree::ReadSwcc(col->s, len);
    return Str(col->s, len);
}

inline size_t value_versioned_array::shallow_size(int ncol) {
    return sizeof(value_versioned_array) + ncol * sizeof(lcdf::inline_string*);
}

inline size_t value_versioned_array::shallow_size() const {
    return shallow_size(dsidle_masstree::LoadSwcc(&ncol_));
}

inline value_versioned_array* value_versioned_array::create(const Json* first, const Json* last, kvtimestamp_t ts, threadinfo& ti) {
    value_versioned_array empty;
    return empty.update(first, last, ts, ti, true);
}

inline value_versioned_array* value_versioned_array::create1(Str value, kvtimestamp_t ts, threadinfo& ti) {
    value_versioned_array* row = (value_versioned_array*) ti.allocate(shallow_size(1), memtag_value);
    dsidle_masstree::StoreSwcc(&row->ts_, ts);
    dsidle_masstree::StoreSwcc(&row->ver_, rowversion());
    dsidle_masstree::StoreSwcc(&row->ncol_, static_cast<short>(1));
    dsidle_masstree::StoreSwcc(&row->ncol_cap_, static_cast<short>(1));
    dsidle_masstree::StoreSwcc(&row->cols_[0], value_array::make_column(value, ti));
    return row;
}

inline void value_versioned_array::deallocate_rcu_after_update(const Json*, const Json*, threadinfo& ti) {
    ti.deallocate_rcu(this, shallow_size(), memtag_value);
}

inline void value_versioned_array::deallocate_after_failed_update(const Json*, const Json*, threadinfo&) {
    always_assert(0);
}

template <typename PARSER>
value_versioned_array*
value_versioned_array::checkpoint_read(PARSER& par, kvtimestamp_t ts,
                                       threadinfo& ti) {
    unsigned ncol;
    par.read_array_header(ncol);
    value_versioned_array* row = make_sized_row(ncol, ts, ti);
    Str col;
    for (unsigned i = 0; i != ncol; i++) {
        par >> col;
        dsidle_masstree::StoreSwcc(&row->cols_[i], value_array::make_column(col, ti));
    }
    return row;
}

template <typename UNPARSER>
void value_versioned_array::checkpoint_write(UNPARSER& unpar) const {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    unpar.write_array_header(ncol);
    for (short i = 0; i != ncol; ++i)
        unpar << col(i);
}

#endif
