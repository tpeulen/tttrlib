// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from scipy/optimize/rectangular_lsap/rectangular_lsap.cpp
// (Copyright (c) 2019 Peter M. Larsen; BSD-3-Clause). See Lsap.h.

#include "Lsap.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>

namespace tttrlib {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

std::ptrdiff_t augmenting_path(std::ptrdiff_t nc, const double* cost, std::vector<double>& u,
                               std::vector<double>& v, std::vector<std::ptrdiff_t>& path,
                               std::vector<std::ptrdiff_t>& row4col, std::vector<double>& shortest,
                               std::ptrdiff_t i, std::vector<bool>& SR, std::vector<bool>& SC,
                               std::vector<std::ptrdiff_t>& remaining, double* p_min_val) {
    double min_val = 0;
    std::ptrdiff_t num_remaining = nc;
    // filled in reverse so a constant cost matrix solves to the identity (scipy #11602)
    for (std::ptrdiff_t it = 0; it < nc; ++it) remaining[it] = nc - it - 1;
    std::fill(SR.begin(), SR.end(), false);
    std::fill(SC.begin(), SC.end(), false);
    std::fill(shortest.begin(), shortest.end(), kInf);
    std::ptrdiff_t sink = -1;
    while (sink == -1) {
        std::ptrdiff_t index = -1;
        double lowest = kInf;
        SR[i] = true;
        for (std::ptrdiff_t it = 0; it < num_remaining; ++it) {
            const std::ptrdiff_t j = remaining[it];
            const double r = min_val + cost[i * nc + j] - u[i] - v[j];
            if (r < shortest[j]) {
                path[j] = i;
                shortest[j] = r;
            }
            // among equal costs prefer a column that ends the path
            if (shortest[j] < lowest || (shortest[j] == lowest && row4col[j] == -1)) {
                lowest = shortest[j];
                index = it;
            }
        }
        min_val = lowest;
        if (min_val == kInf) return -1;  // infeasible
        const std::ptrdiff_t j = remaining[index];
        if (row4col[j] == -1) sink = j;
        else i = row4col[j];
        SC[j] = true;
        remaining[index] = remaining[--num_remaining];
    }
    *p_min_val = min_val;
    return sink;
}

}  // namespace

void linear_sum_assignment(const double* input_cost, int n_rows, int n_cols, bool maximize,
                           std::vector<std::int64_t>& rows, std::vector<std::int64_t>& cols) {
    rows.clear();
    cols.clear();
    if (n_rows < 0 || n_cols < 0) throw std::invalid_argument("linear_sum_assignment: negative shape");
    std::ptrdiff_t nr = n_rows, nc = n_cols;
    if (nr == 0 || nc == 0) return;
    const bool transpose = nc < nr;
    std::vector<double> temp;
    const double* cost = input_cost;
    if (transpose || maximize) {
        temp.resize(static_cast<size_t>(nr * nc));
        if (transpose) {
            for (std::ptrdiff_t i = 0; i < nr; ++i)
                for (std::ptrdiff_t j = 0; j < nc; ++j) temp[j * nr + i] = input_cost[i * nc + j];
            std::swap(nr, nc);
        } else {
            std::copy(input_cost, input_cost + nr * nc, temp.begin());
        }
        if (maximize)
            for (auto& c : temp) c = -c;
        cost = temp.data();
    }
    for (std::ptrdiff_t i = 0; i < nr * nc; ++i)
        if (cost[i] != cost[i] || cost[i] == -kInf)
            throw std::invalid_argument("matrix contains invalid numeric entries");

    std::vector<double> u(nr, 0), v(nc, 0), shortest(nc);
    std::vector<std::ptrdiff_t> path(nc, -1), col4row(nr, -1), row4col(nc, -1), remaining(nc);
    std::vector<bool> SR(nr), SC(nc);
    for (std::ptrdiff_t cur = 0; cur < nr; ++cur) {
        double min_val;
        const std::ptrdiff_t sink =
            augmenting_path(nc, cost, u, v, path, row4col, shortest, cur, SR, SC, remaining, &min_val);
        if (sink < 0) throw std::invalid_argument("cost matrix is infeasible");
        u[cur] += min_val;
        for (std::ptrdiff_t i = 0; i < nr; ++i)
            if (SR[i] && i != cur) u[i] += min_val - shortest[col4row[i]];
        for (std::ptrdiff_t j = 0; j < nc; ++j)
            if (SC[j]) v[j] -= min_val - shortest[j];
        std::ptrdiff_t j = sink;
        while (true) {
            const std::ptrdiff_t i = path[j];
            row4col[j] = i;
            std::swap(col4row[i], j);
            if (i == cur) break;
        }
    }
    rows.resize(static_cast<size_t>(nr));
    cols.resize(static_cast<size_t>(nr));
    if (transpose) {
        std::vector<std::ptrdiff_t> order(nr);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&](std::ptrdiff_t a, std::ptrdiff_t b) { return col4row[a] < col4row[b]; });
        for (std::ptrdiff_t i = 0; i < nr; ++i) {
            rows[i] = col4row[order[i]];
            cols[i] = order[i];
        }
    } else {
        for (std::ptrdiff_t i = 0; i < nr; ++i) {
            rows[i] = i;
            cols[i] = col4row[i];
        }
    }
}

void lsap_solve(const double* cost, int n_rows, int n_cols, int maximize, long long** out, int* n_out) {
    std::vector<std::int64_t> rows, cols;
    linear_sum_assignment(cost, n_rows, n_cols, maximize != 0, rows, cols);
    const size_t n = rows.size();
    long long* buf = static_cast<long long*>(std::malloc(sizeof(long long) * (2 * n ? 2 * n : 1)));
    if (!buf) throw std::bad_alloc();
    for (size_t i = 0; i < n; ++i) {
        buf[i] = rows[i];
        buf[n + i] = cols[i];
    }
    *out = buf;
    *n_out = static_cast<int>(2 * n);
}

}  // namespace tttrlib
