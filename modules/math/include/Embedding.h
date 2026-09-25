// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_EMBEDDING_H
#define TTTRLIB_EMBEDDING_H

// Validation: A/B-TESTED 2026-09-24 -- t-SNE joint probabilities (exact and kNN)
//   vs scikit-learn 1.9 `_joint_probabilities` / `_joint_probabilities_nn`, UMAP
//   fuzzy simplicial set and a/b vs umap-learn `fuzzy_simplicial_set` /
//   `find_ab_params`, from recorded fixtures; embeddings by trustworthiness and
//   KL divergence against both. test/python/misc/test_embedding.py.
//   Register: okf/testing/math-kernel-validation.md

// Embedding.h -- t-SNE and UMAP: non-linear 2-D/3-D embeddings of a point
// cloud (burst parameters, per-pixel feature vectors, ...) in which clusters
// that overlap in any single projection come apart, so they can be gated or
// handed to HDBSCAN for segmentation.
//
// Both are non-parametric: the result is a set of coordinates for the points
// given, nothing is trained or kept. They follow the reference implementations
// step for step where a reference has a single right answer --
//
//   t-SNE (van der Maaten & Hinton 2008; Barnes-Hut: van der Maaten 2014), as
//   scikit-learn implements it: the perplexity binary search on float32 squared
//   distances, symmetric joint P, early exaggeration 12 for 250 iterations at
//   momentum 0.5, then momentum 0.8; delta-bar-delta gains; learning rate
//   "auto" = max(n / exaggeration / 4, 50); PCA initialisation scaled to a
//   first-axis standard deviation of 1e-4.
//
//   UMAP (McInnes, Healy & Melville 2018), as umap-learn implements it: exact
//   k nearest neighbours (the point itself first), smooth_knn_dist (rho, sigma
//   by binary search to log2(k)), membership strengths, fuzzy union, the a/b
//   curve fit from min_dist and spread, the edge pruning below max/n_epochs,
//   epochs-per-sample scheduling, negative sampling with the 4.0 gradient
//   clip and a linearly decaying learning rate.
//
// -- and differ only where no single answer exists: the random streams are
// PCG32 from `seed`, and UMAP's spectral initialisation is a Lanczos solve
// (with full reorthogonalisation) where umap-learn calls ARPACK.
//
// Euclidean distance only. Neighbours are exact (KDTree), so the cost grows
// with the feature count; a few tens of thousands of points in up to ~20
// dimensions is the intended range.

#include <cstdint>

namespace tttrlib {

// ---- t-SNE -----------------------------------------------------------------

/// Joint probabilities P over all pairs (exact t-SNE): the conditional
/// affinities at the given perplexity, symmetrised and normalised, floored at
/// machine epsilon. Returns the dense `n x n` matrix (zero diagonal) that
/// scikit-learn's `squareform(_joint_probabilities(...))` gives.
void tsne_joint_probabilities(const double* data, int n_samples, int n_features,
                              double perplexity,
                              double** out_P, int* n_rows, int* n_cols);

/// Joint probabilities over the `min(n-1, floor(3*perplexity+1))` nearest
/// neighbours (Barnes-Hut t-SNE), as a sparse symmetric matrix in COO form,
/// entries sorted by (row, col), summing to one.
void tsne_joint_probabilities_nn(const double* data, int n_samples, int n_features,
                                 double perplexity,
                                 long long** out_row, int* n_out_row,
                                 long long** out_col, int* n_out_col,
                                 double** out_value, int* n_out_value);

/// The t-SNE embedding.
///
/// `init` (`n_samples x n_components`) starts the optimisation; pass an empty
/// array for the PCA initialisation. `learning_rate <= 0` means "auto".
/// `method` 0 is exact (O(n^2) per iteration), 1 is Barnes-Hut with opening
/// angle `angle` (n_components 2 or 3 only). Returns the embedding
/// (`n_samples x n_components`); `kl_divergence` is the final KL divergence
/// and `n_iter` the index of the last iteration run.
void tsne_embed(const double* data, int n_samples, int n_features,
                const double* init, int init_rows, int init_cols,
                int n_components, double perplexity, double early_exaggeration,
                double learning_rate, int max_iter, int n_iter_without_progress,
                double min_grad_norm, int method, double angle,
                double** out_embedding, int* n_out_rows, int* n_out_cols,
                double* kl_divergence, int* n_iter);

// ---- UMAP ------------------------------------------------------------------

/// The `a`, `b` of the low-dimensional membership 1 / (1 + a d^(2b)), fitted
/// by least squares to the target curve umap-learn fits (300 points on
/// [0, 3*spread]: one below `min_dist`, exp(-(d - min_dist)/spread) above).
void umap_find_ab_params(double spread, double min_dist, double* a, double* b);

/// The fuzzy simplicial set (the symmetric UMAP graph) over the exact
/// `n_neighbors` nearest neighbours, as COO entries sorted by (row, col).
/// `set_op_mix_ratio` 1 is the fuzzy union, 0 the intersection.
void umap_fuzzy_graph(const double* data, int n_samples, int n_features,
                      int n_neighbors, double local_connectivity,
                      double set_op_mix_ratio,
                      long long** out_row, int* n_out_row,
                      long long** out_col, int* n_out_col,
                      double** out_value, int* n_out_value);

/// UMAP's spectral initialisation of a graph (COO, both directions): the
/// eigenvectors 2..n_components+1 of its normalised Laplacian, as umap-learn's
/// `spectral_layout`, by thick-restart Lanczos. Throws when the solve does not
/// converge (a disconnected or degenerate graph); `umap_embed` then falls back
/// to a random start, as umap-learn does.
void umap_spectral_layout(const long long* row, int n_row, const long long* col, int n_col,
                          const double* value, int n_value, int n_samples, int n_components,
                          int seed, double** out_layout, int* n_out_rows, int* n_out_cols);

/// The UMAP embedding.
///
/// `init` (`n_samples x n_components`) starts the optimisation; pass an empty
/// array for the spectral initialisation. `n_epochs <= 0` means 500 up to
/// 10000 points and 200 above; `a <= 0` or `b <= 0` fits both from
/// `min_dist` and `spread`. `seed` drives the initial jitter and the negative
/// sampling. Returns the embedding (`n_samples x n_components`).
void umap_embed(const double* data, int n_samples, int n_features,
                const double* init, int init_rows, int init_cols,
                int n_components, int n_neighbors, double min_dist, double spread,
                int n_epochs, double learning_rate, double negative_sample_rate,
                double repulsion_strength, double local_connectivity,
                double set_op_mix_ratio, double a, double b, int seed,
                double** out_embedding, int* n_out_rows, int* n_out_cols);

/// Trustworthiness (Venna & Kaski 2001), as scikit-learn's
/// `manifold.trustworthiness`: how far the `n_neighbors` nearest neighbours in
/// the embedding are from being neighbours in the data. 1 is perfect.
double embedding_trustworthiness(const double* data, int n_samples, int n_features,
                                 const double* embedding, int emb_rows, int emb_cols,
                                 int n_neighbors);

}  // namespace tttrlib

#endif  // TTTRLIB_EMBEDDING_H
