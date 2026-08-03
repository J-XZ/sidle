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
#include "kvrow.hh"
#include "value_versioned_array.hh"
#include <string.h>

value_versioned_array* value_versioned_array::make_sized_row(int ncol, kvtimestamp_t ts, threadinfo& ti) {
    value_versioned_array* row = (value_versioned_array*) ti.allocate(shallow_size(ncol), memtag_value);
    dsidle_masstree::StoreSwcc(&row->ts_, ts);
    dsidle_masstree::StoreSwcc(&row->ver_, rowversion());
    dsidle_masstree::StoreSwcc(&row->ncol_, static_cast<short>(ncol));
    dsidle_masstree::StoreSwcc(&row->ncol_cap_, static_cast<short>(ncol));
    memset(row->cols_, 0, sizeof(row->cols_[0]) * ncol);
    dsidle_masstree::WriteSwcc(row->cols_, sizeof(row->cols_[0]) * ncol);
    return row;
}

void value_versioned_array::snapshot(value_versioned_array*& storage,
                                     const std::vector<index_type>& f, threadinfo& ti) const {
    const auto current_ncol = dsidle_masstree::LoadSwcc(&ncol_);
    const auto current_ts = dsidle_masstree::LoadSwcc(&ts_);
    if (!storage || dsidle_masstree::LoadSwcc(&storage->ncol_cap_) < current_ncol) {
        if (storage)
            storage->deallocate(ti);
        storage = make_sized_row(current_ncol, current_ts, ti);
    }
    dsidle_masstree::StoreSwcc(&storage->ncol_, static_cast<short>(current_ncol));
    rowversion v1 = ver_.stable();
    while (1) {
        if (f.size() == 1) {
            auto* source = dsidle_masstree::LoadSwcc(&cols_[f[0]]);
            dsidle_masstree::StoreSwcc(&storage->cols_[f[0]], source);
        } else {
            const auto bytes = sizeof(cols_[0]) * current_ncol;
            memcpy(storage->cols_, cols_, bytes);
            dsidle_masstree::ReadSwcc(cols_, bytes);
            dsidle_masstree::WriteSwcc(storage->cols_, bytes);
        }
        rowversion v2 = ver_.stable();
        if (!v1.has_changed(v2))
            break;
        v1 = v2;
    }
}

value_versioned_array*
value_versioned_array::update(const Json* first, const Json* last,
                              kvtimestamp_t ts, threadinfo& ti,
                              bool always_copy) {
    int ncol = last[-2].as_u() + 1;
    const auto old_ncol = static_cast<int>(dsidle_masstree::LoadSwcc(&ncol_));
    const auto old_ncol_cap = static_cast<int>(
        dsidle_masstree::LoadSwcc(&ncol_cap_));
    value_versioned_array* row;
    if (ncol > old_ncol_cap || always_copy) {
        row = (value_versioned_array*) ti.allocate(shallow_size(ncol), memtag_value);
        dsidle_masstree::StoreSwcc(&row->ts_, ts);
        dsidle_masstree::StoreSwcc(&row->ver_, rowversion());
        dsidle_masstree::StoreSwcc(&row->ncol_, static_cast<short>(ncol));
        dsidle_masstree::StoreSwcc(&row->ncol_cap_, static_cast<short>(ncol));
        const auto bytes = sizeof(cols_[0]) * old_ncol;
        memcpy(row->cols_, cols_, bytes);
        dsidle_masstree::ReadSwcc(cols_, bytes);
        dsidle_masstree::WriteSwcc(row->cols_, bytes);
    } else
        row = this;
    if (ncol > old_ncol) {
        const auto bytes = sizeof(cols_[0]) * (ncol - old_ncol);
        memset(row->cols_ + old_ncol, 0, bytes);
        dsidle_masstree::WriteSwcc(row->cols_ + old_ncol, bytes);
    }

    if (row == this) {
        ver_.setdirty();
        fence();
    }
    if (dsidle_masstree::LoadSwcc(&row->ncol_) < ncol)
        dsidle_masstree::StoreSwcc(&row->ncol_, static_cast<short>(ncol));

    for (; first != last; first += 2) {
        unsigned idx = first[0].as_u();
        value_array::deallocate_column_rcu(
            dsidle_masstree::LoadSwcc(&row->cols_[idx]), ti);
        dsidle_masstree::StoreSwcc(&row->cols_[idx],
                                   value_array::make_column(first[1].as_s(), ti));
    }

    if (row == this) {
        fence();
        ver_.clearandbump();
    }
    return row;
}

void value_versioned_array::deallocate(threadinfo &ti) {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    for (short i = 0; i < ncol; ++i)
        value_array::deallocate_column(dsidle_masstree::LoadSwcc(&cols_[i]), ti);
    ti.deallocate(this, shallow_size(), memtag_value);
}

void value_versioned_array::deallocate_rcu(threadinfo &ti) {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    for (short i = 0; i < ncol; ++i)
        value_array::deallocate_column_rcu(
            dsidle_masstree::LoadSwcc(&cols_[i]), ti);
    ti.deallocate_rcu(this, shallow_size(), memtag_value);
}
