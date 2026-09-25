// SPDX-License-Identifier: BSD-3-Clause
// Helpers shared by the t-SNE and UMAP kernels (Tsne.cpp, Umap.cpp), the
// trustworthiness score both are judged by, and their registry entries.

#include "Embedding.h"
#include "EmbeddingDetail.h"

#include "Cluster.h"
#include "Registry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tttrlib {
namespace embedding_detail {

void check_data(const double* X, int n, int d, const char* what) {
    if (X == nullptr || n <= 0 || d <= 0)
        throw std::invalid_argument(std::string(what) + ": data must be a non-empty 2-D array");
    for (long long i = 0; i < static_cast<long long>(n) * d; ++i)
        if (!std::isfinite(X[i]))
            throw std::invalid_argument(std::string(what) + ": data contains NaN or infinity");
}

void sym_eig(std::vector<double> A, int n,
             std::vector<double>& evals, std::vector<double>& evecs) {
    std::vector<double> V(static_cast<std::size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) V[static_cast<std::size_t>(i) * n + i] = 1.0;
    double total = 0.0;
    for (double a : A) total += a * a;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += A[p * n + q] * A[p * n + q];
        if (off <= 1e-30 * (total > 0 ? total : 1.0)) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = A[p * n + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (A[q * n + q] - A[p * n + p]) / (2.0 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {           // A <- A J
                    const double akp = A[k * n + p], akq = A[k * n + q];
                    A[k * n + p] = c * akp - s * akq;
                    A[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {           // A <- J^T A
                    const double apk = A[p * n + k], aqk = A[q * n + k];
                    A[p * n + k] = c * apk - s * aqk;
                    A[q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {           // V <- V J
                    const double vkp = V[k * n + p], vkq = V[k * n + q];
                    V[k * n + p] = c * vkp - s * vkq;
                    V[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return A[a * n + a] > A[b * n + b]; });
    evals.assign(n, 0.0);
    evecs.assign(static_cast<std::size_t>(n) * n, 0.0);
    for (int k = 0; k < n; ++k) {
        evals[k] = A[order[k] * n + order[k]];
        for (int i = 0; i < n; ++i) evecs[i * n + k] = V[i * n + order[k]];
    }
}

std::vector<double> pca_project(const double* X, int n, int d, int n_components) {
    if (n_components > d)
        throw std::invalid_argument("PCA initialisation needs n_components <= n_features");
    std::vector<double> mean(d, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < d; ++j) mean[j] += X[static_cast<std::size_t>(i) * d + j];
    for (double& m : mean) m /= n;
    std::vector<double> C(static_cast<std::size_t>(d) * d, 0.0);
    for (int i = 0; i < n; ++i) {
        const double* x = X + static_cast<std::size_t>(i) * d;
        for (int a = 0; a < d; ++a) {
            const double xa = x[a] - mean[a];
            for (int b = a; b < d; ++b) C[a * d + b] += xa * (x[b] - mean[b]);
        }
    }
    for (int a = 0; a < d; ++a)
        for (int b = 0; b < a; ++b) C[a * d + b] = C[b * d + a];
    std::vector<double> evals, evecs;
    sym_eig(C, d, evals, evecs);
    for (int k = 0; k < n_components; ++k) {            // svd_flip: largest |loading| > 0
        int arg = 0;
        for (int j = 1; j < d; ++j)
            if (std::fabs(evecs[j * d + k]) > std::fabs(evecs[arg * d + k])) arg = j;
        if (evecs[arg * d + k] < 0)
            for (int j = 0; j < d; ++j) evecs[j * d + k] = -evecs[j * d + k];
    }
    std::vector<double> Y(static_cast<std::size_t>(n) * n_components, 0.0);
    for (int i = 0; i < n; ++i) {
        const double* x = X + static_cast<std::size_t>(i) * d;
        for (int k = 0; k < n_components; ++k) {
            double s = 0.0;
            for (int j = 0; j < d; ++j) s += (x[j] - mean[j]) * evecs[j * d + k];
            Y[static_cast<std::size_t>(i) * n_components + k] = s;
        }
    }
    return Y;
}

void knn(const double* X, int n, int d, int k,
         std::vector<int>& index, std::vector<double>& distance) {
    if (k > n) throw std::invalid_argument("more neighbours requested than there are points");
    KDTree tree(X, n, d, KDTree::default_leaf_size(d));
    index.assign(static_cast<std::size_t>(n) * k, 0);
    distance.assign(static_cast<std::size_t>(n) * k, 0.0);
    std::vector<int> idx(k);
    std::vector<double> sq(k);
    for (int i = 0; i < n; ++i) {
        tree.query(X + static_cast<std::size_t>(i) * d, k, idx.data(), sq.data());
        // The point itself first, even among exact duplicates at distance 0.
        int self = -1;
        for (int j = 0; j < k; ++j) if (idx[j] == i) { self = j; break; }
        if (self < 0) self = k - 1, idx[self] = i, sq[self] = 0.0;
        for (int j = self; j > 0; --j) std::swap(idx[j], idx[j - 1]), std::swap(sq[j], sq[j - 1]);
        for (int j = 0; j < k; ++j) {
            index[static_cast<std::size_t>(i) * k + j] = idx[j];
            distance[static_cast<std::size_t>(i) * k + j] = std::sqrt(std::max(sq[j], 0.0));
        }
    }
}

}  // namespace embedding_detail

double embedding_trustworthiness(const double* data, int n_samples, int n_features,
                                 const double* embedding, int emb_rows, int emb_cols,
                                 int n_neighbors) {
    using namespace embedding_detail;
    check_data(data, n_samples, n_features, "embedding_trustworthiness");
    check_data(embedding, emb_rows, emb_cols, "embedding_trustworthiness");
    if (emb_rows != n_samples)
        throw std::invalid_argument("embedding_trustworthiness: one embedded row per data row");
    const int n = n_samples, k = n_neighbors;
    if (k < 1 || k >= n / 2.0)
        throw std::invalid_argument("embedding_trustworthiness: need 1 <= n_neighbors < n_samples / 2");
    std::vector<int> nn_idx;
    std::vector<double> nn_dist;
    knn(embedding, n, emb_cols, k + 1, nn_idx, nn_dist);   // column 0 is the point itself
    std::vector<int> rank(n), order(n);
    std::vector<double> dist(n);
    double t = 0.0;
    for (int i = 0; i < n; ++i) {
        const double* xi = data + static_cast<std::size_t>(i) * n_features;
        for (int j = 0; j < n; ++j) {
            const double* xj = data + static_cast<std::size_t>(j) * n_features;
            double s = 0.0;
            for (int f = 0; f < n_features; ++f) s += (xi[f] - xj[f]) * (xi[f] - xj[f]);
            dist[j] = (j == i) ? std::numeric_limits<double>::infinity() : s;
        }
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return dist[a] < dist[b]; });
        for (int r = 0; r < n; ++r) rank[order[r]] = r + 1;      // 1-based
        for (int j = 1; j <= k; ++j) {
            const int r = rank[nn_idx[static_cast<std::size_t>(i) * (k + 1) + j]] - k;
            if (r > 0) t += r;
        }
    }
    return 1.0 - t * (2.0 / (static_cast<double>(n) * k * (2.0 * n - 3.0 * k - 1.0)));
}

// ---- registry entries (Registry.h, core): declared next to the code, registered
// when this library loads; a static consumer links the archive whole.
namespace {
const char* const kTsneEntry = R"JSON({
  "name": "tsne_embedding",
  "label": "t-SNE embedding",
  "summary": "t-distributed stochastic neighbour embedding (exact or Barnes-Hut) of a point cloud into 2 or 3 dimensions, for gating and segmentation.",
  "description": "van der Maaten & Hinton's t-SNE as scikit-learn implements it: per-point Gaussian affinities calibrated to a perplexity by binary search, symmetrised joint probabilities, a Student-t kernel in the embedding, early exaggeration then momentum gradient descent with delta-bar-delta gains, PCA initialisation. Exact (O(n^2) per iteration) or Barnes-Hut (van der Maaten 2014) with an opening angle. Rows are anything with a feature vector -- bursts, per-pixel lifetimes and phasors -- and clusters that overlap in every single projection come apart in the embedding, where they can be gated or passed to HDBSCAN. A/B-tested against scikit-learn (test_embedding.py).",
  "operation_type": "analysis",
  "method": "tsne",
  "params_schema": {
    "type": "object",
    "properties": {
      "n_components": {"type": "integer", "title": "Dimensions", "default": 2},
      "perplexity": {"type": "number", "title": "Perplexity", "default": 30.0},
      "early_exaggeration": {"type": "number", "title": "Early exaggeration", "default": 12.0},
      "max_iter": {"type": "integer", "title": "Iterations", "default": 1000},
      "angle": {"type": "number", "title": "Barnes-Hut angle", "default": 0.5}
    }
  },
  "inputs": {"required": ["points"]},
  "outputs": {"columns": ["embedding"]},
  "row_grain": "molecule",
  "references": [
    {"type": "journal", "authors": "van der Maaten, L., Hinton, G.", "title": "Visualizing data using t-SNE", "journal": "J Mach Learn Res", "year": 2008, "volume": "9", "pages": "2579-2605"},
    {"type": "journal", "authors": "van der Maaten, L.", "title": "Accelerating t-SNE using tree-based algorithms", "journal": "J Mach Learn Res", "year": 2014, "volume": "15", "pages": "3221-3245"}
  ],
  "api": [
    "tsne", "tsne_embed", "tsne_joint_probabilities", "tsne_joint_probabilities_nn",
    "embedding_trustworthiness"
  ],
  "can_replay": false
})JSON";

const char* const kUmapEntry = R"JSON({
  "name": "umap_embedding",
  "label": "UMAP embedding",
  "summary": "Uniform manifold approximation and projection of a point cloud into a few dimensions, for gating and segmentation; with helpers that turn an image's per-pixel features into points and labels back into a label image.",
  "description": "McInnes, Healy & Melville's UMAP as umap-learn implements it: exact k nearest neighbours, per-point connectivity rho and bandwidth sigma by binary search to log2(k), membership strengths combined by fuzzy union, the low-dimensional curve 1/(1+a d^2b) fitted from min_dist and spread, spectral initialisation of the normalised graph Laplacian, and stochastic gradient descent with negative sampling. Deterministic for a given seed. image_features_to_points and points_to_label_image carry per-pixel feature stacks (lifetime, intensity, phasor g/s) into the embedding and cluster labels back into a segmentation. A/B-tested against umap-learn (test_embedding.py).",
  "operation_type": "analysis",
  "method": "umap",
  "params_schema": {
    "type": "object",
    "properties": {
      "n_components": {"type": "integer", "title": "Dimensions", "default": 2},
      "n_neighbors": {"type": "integer", "title": "Neighbours", "default": 15},
      "min_dist": {"type": "number", "title": "Minimum distance", "default": 0.1},
      "spread": {"type": "number", "title": "Spread", "default": 1.0},
      "n_epochs": {"type": "integer", "title": "Epochs (0: automatic)", "default": 0}
    }
  },
  "inputs": {"required": ["points"]},
  "outputs": {"columns": ["embedding"]},
  "row_grain": "molecule",
  "references": [
    {"type": "journal", "authors": "McInnes, L., Healy, J., Melville, J.", "title": "UMAP: Uniform manifold approximation and projection for dimension reduction", "journal": "arXiv", "year": 2018, "volume": "1802.03426", "pages": "1-63"}
  ],
  "api": [
    "umap", "umap_embed", "umap_fuzzy_graph", "umap_find_ab_params", "umap_spectral_layout",
    "image_features_to_points", "points_to_label_image"
  ],
  "can_replay": false
})JSON";

bool register_embedding_entries() {
    tttrlib::register_algorithm_json("math", "tsne_embedding", kTsneEntry);
    tttrlib::register_algorithm_json("math", "umap_embedding", kUmapEntry);
    return true;
}
const bool kEmbeddingRegistered = register_embedding_entries();
}  // namespace

}  // namespace tttrlib
