// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_SPATIALOPS_H
#define TTTRLIB_SPATIALOPS_H
// SpatialOps.h -- batch queries on a KDTree for the language bindings: many
// query points in, flat malloc'd arrays out (the ImageOps.h pattern). The
// scipy-shaped `cKDTree` methods (`query`, `query_ball_point`,
// `query_pairs`) are written over these in Python.

#include "Cluster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace tttrlib {

namespace spatial_detail {
template <typename T>
T* alloc(size_t n) {
    void* p = std::malloc(sizeof(T) * (n ? n : 1));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
}
}  // namespace spatial_detail

/// k nearest neighbours of each query row: `n_points * k` indices and
/// Euclidean distances, each row ascending in (distance, index). Rows past
/// the tree's size are padded with index `n_samples` and distance `inf`, as
/// scipy pads them.
inline void kdtree_query(const KDTree& tree, const double* points, int n_points, int n_dims, int k,
                         long long** out_index, int* n_out_index, double** out_dist, int* n_out_dist) {
    if (n_dims != tree.n_features()) throw std::invalid_argument("KDTree.query: dimension mismatch");
    if (k < 1) throw std::invalid_argument("KDTree.query: k must be >= 1");
    const size_t n = static_cast<size_t>(n_points) * static_cast<size_t>(k);
    long long* idx = spatial_detail::alloc<long long>(n);
    double* dist = spatial_detail::alloc<double>(n);
    const int kk = std::min(k, tree.n_samples());
    std::vector<int> ix(static_cast<size_t>(std::max(kk, 1)));
    std::vector<double> sq(static_cast<size_t>(std::max(kk, 1)));
    std::vector<int> order(static_cast<size_t>(std::max(kk, 1)));
    for (int q = 0; q < n_points; ++q) {
        if (kk > 0) tree.query(points + static_cast<size_t>(q) * n_dims, kk, ix.data(), sq.data());
        std::iota(order.begin(), order.begin() + kk, 0);
        std::sort(order.begin(), order.begin() + kk, [&](int a, int b) {
            return sq[a] < sq[b] || (sq[a] == sq[b] && ix[a] < ix[b]);
        });
        for (int j = 0; j < k; ++j) {
            const size_t o = static_cast<size_t>(q) * k + j;
            if (j < kk) {
                idx[o] = ix[order[j]];
                dist[o] = std::sqrt(sq[order[j]]);
            } else {
                idx[o] = tree.n_samples();
                dist[o] = INFINITY;
            }
        }
    }
    *out_index = idx;
    *n_out_index = static_cast<int>(n);
    *out_dist = dist;
    *n_out_dist = static_cast<int>(n);
}

/// Neighbours within `r` of each query row in CSR form: `offsets` has
/// `n_points + 1` entries, the hits of row q are `index[offsets[q]:offsets[q+1]]`,
/// sorted ascending.
inline void kdtree_query_ball_point(const KDTree& tree, const double* points, int n_points, int n_dims, double r,
                                    double p, long long** out_offsets, int* n_out_offsets, long long** out_hits,
                                    int* n_out_hits) {
    if (n_dims != tree.n_features()) throw std::invalid_argument("KDTree.query_ball_point: dimension mismatch");
    std::vector<long long> offsets(static_cast<size_t>(n_points) + 1, 0), hits;
    std::vector<int> buf;
    for (int q = 0; q < n_points; ++q) {
        buf.clear();
        tree.query_radius(points + static_cast<size_t>(q) * n_dims, r, p, buf);
        std::sort(buf.begin(), buf.end());
        hits.insert(hits.end(), buf.begin(), buf.end());
        offsets[q + 1] = static_cast<long long>(hits.size());
    }
    long long* o = spatial_detail::alloc<long long>(offsets.size());
    long long* h = spatial_detail::alloc<long long>(hits.size());
    std::copy(offsets.begin(), offsets.end(), o);
    std::copy(hits.begin(), hits.end(), h);
    *out_offsets = o;
    *n_out_offsets = static_cast<int>(offsets.size());
    *out_hits = h;
    *n_out_hits = static_cast<int>(hits.size());
}

/// All pairs `i < j` within `r`, flat `[i0, j0, i1, j1, ...]`, sorted.
inline void kdtree_query_pairs(const KDTree& tree, double r, double p, long long** out_pairs, int* n_out_pairs) {
    const std::vector<int> pairs = tree.query_pairs(r, p);
    long long* o = spatial_detail::alloc<long long>(pairs.size());
    std::copy(pairs.begin(), pairs.end(), o);
    *out_pairs = o;
    *n_out_pairs = static_cast<int>(pairs.size());
}

}  // namespace tttrlib

#endif  // TTTRLIB_SPATIALOPS_H
