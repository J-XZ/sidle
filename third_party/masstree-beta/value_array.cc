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
#include "value_array.hh"
#include <string.h>

value_array* value_array::make_sized_row(int ncol, kvtimestamp_t ts,
                                         threadinfo& ti) {
    value_array *tv;
    tv = (value_array *) ti.allocate(shallow_size(ncol), memtag_value);
    dsidle_masstree::StoreSwcc(&tv->ts_, ts);
    dsidle_masstree::StoreSwcc(&tv->ncol_, static_cast<short>(ncol));
    memset(tv->cols_, 0, sizeof(tv->cols_[0]) * ncol);
    dsidle_masstree::WriteSwcc(tv->cols_, sizeof(tv->cols_[0]) * ncol);
    return tv;
}

value_array* value_array::update(const Json* first, const Json* last,
                                 kvtimestamp_t ts, threadinfo& ti) const {
    masstree_precondition(ts >= timestamp());
    const auto old_ncol = static_cast<unsigned>(ncol());
    unsigned ncol = std::max(int(old_ncol), int(last[-2].as_i()) + 1);
    value_array* row = (value_array*) ti.allocate(shallow_size(ncol), memtag_value);
    dsidle_masstree::StoreSwcc(&row->ts_, ts);
    dsidle_masstree::StoreSwcc(&row->ncol_, static_cast<short>(ncol));
    memcpy(row->cols_, cols_, old_ncol * sizeof(cols_[0]));
    dsidle_masstree::ReadSwcc(cols_, old_ncol * sizeof(cols_[0]));
    dsidle_masstree::WriteSwcc(row->cols_, old_ncol * sizeof(cols_[0]));
    memset(row->cols_ + old_ncol, 0,
           (ncol - old_ncol) * sizeof(cols_[0]));
    dsidle_masstree::WriteSwcc(row->cols_ + old_ncol,
                               (ncol - old_ncol) * sizeof(cols_[0]));
    for (; first != last; first += 2)
        dsidle_masstree::StoreSwcc(&row->cols_[first[0].as_u()],
                                   make_column(first[1].as_s(), ti));
    return row;
}

void value_array::deallocate(threadinfo& ti) {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    for (short i = 0; i < ncol; ++i)
        deallocate_column(dsidle_masstree::LoadSwcc(&cols_[i]), ti);
    ti.deallocate(this, shallow_size(), memtag_value);
}

void value_array::deallocate_rcu(threadinfo& ti) {
    const auto ncol = dsidle_masstree::LoadSwcc(&ncol_);
    for (short i = 0; i < ncol; ++i)
        deallocate_column_rcu(dsidle_masstree::LoadSwcc(&cols_[i]), ti);
    ti.deallocate_rcu(this, shallow_size(), memtag_value);
}

void value_array::deallocate_rcu_after_update(const Json* first, const Json* last, threadinfo& ti) {
    const auto old_ncol = static_cast<unsigned>(this->ncol());
    for (; first != last && first[0].as_u() < old_ncol; first += 2)
        deallocate_column_rcu(
            dsidle_masstree::LoadSwcc(&cols_[first[0].as_u()]), ti);
    ti.deallocate_rcu(this, shallow_size(), memtag_value);
}

void value_array::deallocate_after_failed_update(const Json* first, const Json* last, threadinfo& ti) {
    for (; first != last; first += 2)
        deallocate_column(
            dsidle_masstree::LoadSwcc(&cols_[first[0].as_u()]), ti);
    ti.deallocate(this, shallow_size(), memtag_value);
}
