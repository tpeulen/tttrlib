// SPDX-License-Identifier: BSD-3-Clause
// Shared by Tsne.cpp, Umap.cpp and Embedding.cpp; not a public header.
#ifndef TTTRLIB_EMBEDDING_DETAIL_H
#define TTTRLIB_EMBEDDING_DETAIL_H

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace tttrlib {
namespace embedding_detail {

/// Eigen-decomposition of a small symmetric matrix by cyclic Jacobi. `A` is
/// row-major `n x n`. Eigenvalues come back in descending order; eigenvector
/// `k` is column `k` of the row-major `n x n` `evecs`.
void sym_eig(std::vector<double> A, int n,
             std::vector<double>& evals, std::vector<double>& evecs);

/// The data projected on its first `n_components` principal axes, each axis
/// signed so its largest-magnitude loading is positive (scikit-learn's
/// svd_flip convention). Row-major `n x n_components`.
std::vector<double> pca_project(const double* X, int n, int d, int n_components);

/// Exact k nearest neighbours of every point, Euclidean (not squared)
/// distances ascending, the point itself first. Row-major `n x k`.
void knn(const double* X, int n, int d, int k,
         std::vector<int>& index, std::vector<double>& distance);

/// Throws std::invalid_argument naming `what` unless 0 < n and 0 < d.
void check_data(const double* X, int n, int d, const char* what);

/// Copies `v` into a malloc'd buffer: the ARGOUTVIEWM contract, whose
/// wrappers (numpy.i free_cap, rarrays.i) release it with free().
template <typename T, typename U>
T* to_new_buffer(const std::vector<U>& v) {
    T* out = static_cast<T*>(std::malloc(sizeof(T) * (v.empty() ? 1 : v.size())));
    if (!out) throw std::bad_alloc();
    for (std::size_t i = 0; i < v.size(); ++i) out[i] = static_cast<T>(v[i]);
    return out;
}

}  // namespace embedding_detail
}  // namespace tttrlib

#endif  // TTTRLIB_EMBEDDING_DETAIL_H
