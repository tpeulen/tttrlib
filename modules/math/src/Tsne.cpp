// SPDX-License-Identifier: BSD-3-Clause
// t-SNE, exact and Barnes-Hut, following scikit-learn's sklearn/manifold/_t_sne.py
// (_joint_probabilities, _joint_probabilities_nn, _kl_divergence,
// _kl_divergence_bh, _gradient_descent, TSNE._tsne) and _utils.pyx
// (_binary_search_perplexity) step for step. See Embedding.h.

#include "Embedding.h"
#include "EmbeddingDetail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace tttrlib {
namespace {

using embedding_detail::check_data;
using embedding_detail::to_new_buffer;

constexpr double kMachineEpsilon = std::numeric_limits<double>::epsilon();
constexpr double kFloat32Tiny = 1.1754943508222875e-38;   // np.finfo(np.float32).tiny
constexpr int kExplorationIter = 250;
constexpr int kIterCheck = 50;

/// sklearn _utils._binary_search_perplexity. `sqd` holds float32-rounded
/// squared distances, `k` per row; `using_neighbors` is true for the kNN form
/// (every column is a neighbour), false for the exact n x n form (column i of
/// row i is the point itself and stays zero).
std::vector<double> binary_search_perplexity(const std::vector<double>& sqd, int n, int k,
                                             double perplexity, bool using_neighbors) {
    const int n_steps = 100;
    const double tol = 1e-5, eps_dbl = 1e-8;
    const double desired_entropy = std::log(perplexity);
    std::vector<double> P(static_cast<std::size_t>(n) * k, 0.0);
    for (int i = 0; i < n; ++i) {
        double beta_min = -std::numeric_limits<double>::infinity();
        double beta_max = std::numeric_limits<double>::infinity();
        double beta = 1.0;
        double* Pi = &P[static_cast<std::size_t>(i) * k];
        const double* di = &sqd[static_cast<std::size_t>(i) * k];
        for (int l = 0; l < n_steps; ++l) {
            double sum_Pi = 0.0;
            for (int j = 0; j < k; ++j) {
                if (j != i || using_neighbors) {
                    Pi[j] = std::exp(-di[j] * beta);
                    sum_Pi += Pi[j];
                }
            }
            if (sum_Pi == 0.0) sum_Pi = eps_dbl;
            double sum_disti_Pi = 0.0;
            for (int j = 0; j < k; ++j) {
                Pi[j] /= sum_Pi;
                sum_disti_Pi += di[j] * Pi[j];
            }
            const double entropy = std::log(sum_Pi) + beta * sum_disti_Pi;
            const double entropy_diff = entropy - desired_entropy;
            if (std::fabs(entropy_diff) <= tol) break;
            if (entropy_diff > 0.0) {
                beta_min = beta;
                beta = std::isinf(beta_max) ? beta * 2.0 : (beta + beta_max) / 2.0;
            } else {
                beta_max = beta;
                beta = std::isinf(beta_min) ? beta / 2.0 : (beta + beta_min) / 2.0;
            }
        }
    }
    return P;
}

inline double sq_dist(const double* a, const double* b, int d) {
    double s = 0.0;
    for (int f = 0; f < d; ++f) s += (a[f] - b[f]) * (a[f] - b[f]);
    return s;
}

/// Dense symmetric joint P (zero diagonal), sklearn _joint_probabilities.
std::vector<double> joint_p_exact(const double* X, int n, int d, double perplexity) {
    std::vector<double> sqd(static_cast<std::size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j)   // sklearn casts the squared distances to float32
                sqd[static_cast<std::size_t>(i) * n + j] = static_cast<double>(static_cast<float>(
                    sq_dist(X + static_cast<std::size_t>(i) * d, X + static_cast<std::size_t>(j) * d, d)));
    std::vector<double> cond = binary_search_perplexity(sqd, n, n, perplexity, false);
    std::vector<double> P(static_cast<std::size_t>(n) * n, 0.0);
    double sum_P = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double v = cond[static_cast<std::size_t>(i) * n + j] + cond[static_cast<std::size_t>(j) * n + i];
            P[static_cast<std::size_t>(i) * n + j] = v;
            sum_P += 2.0 * v;
        }
    sum_P = std::max(sum_P, kMachineEpsilon);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double v = std::max(P[static_cast<std::size_t>(i) * n + j] / sum_P, kMachineEpsilon);
            P[static_cast<std::size_t>(i) * n + j] = v;
            P[static_cast<std::size_t>(j) * n + i] = v;
        }
    return P;
}

struct Csr {
    std::vector<int> indptr, indices;
    std::vector<double> data;
};

/// Sparse symmetric joint P over the kNN graph, sklearn _joint_probabilities_nn.
Csr joint_p_nn(const double* X, int n, int d, double perplexity) {
    const int k = std::min(n - 1, static_cast<int>(3.0 * perplexity + 1.0));
    if (k < 1) throw std::invalid_argument("t-SNE needs at least two points");
    std::vector<int> idx;
    std::vector<double> dist;
    embedding_detail::knn(X, n, d, k + 1, idx, dist);    // column 0 is the point itself
    std::vector<double> sqd(static_cast<std::size_t>(n) * k);
    std::vector<int> nbr(static_cast<std::size_t>(n) * k);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < k; ++j) {
            const double e = dist[static_cast<std::size_t>(i) * (k + 1) + j + 1];
            sqd[static_cast<std::size_t>(i) * k + j] = static_cast<double>(static_cast<float>(e * e));
            nbr[static_cast<std::size_t>(i) * k + j] = idx[static_cast<std::size_t>(i) * (k + 1) + j + 1];
        }
    std::vector<double> cond = binary_search_perplexity(sqd, n, k, perplexity, true);
    // P + P^T, accumulated per row, then normalised by the total.
    std::vector<std::vector<std::pair<int, double>>> rows(n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < k; ++j) {
            const int c = nbr[static_cast<std::size_t>(i) * k + j];
            const double v = cond[static_cast<std::size_t>(i) * k + j];
            rows[i].emplace_back(c, v);
            rows[c].emplace_back(i, v);
        }
    Csr P;
    P.indptr.assign(n + 1, 0);
    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        auto& r = rows[i];
        std::sort(r.begin(), r.end());
        for (std::size_t a = 0; a < r.size();) {
            std::size_t b = a;
            double v = 0.0;
            while (b < r.size() && r[b].first == r[a].first) v += r[b++].second;
            P.indices.push_back(r[a].first);
            P.data.push_back(v);
            total += v;
            a = b;
        }
        P.indptr[i + 1] = static_cast<int>(P.indices.size());
    }
    total = std::max(total, kMachineEpsilon);
    for (double& v : P.data) v /= total;
    return P;
}

// ---- objectives --------------------------------------------------------------

/// sklearn _kl_divergence: KL(P||Q) and its gradient for the exact method.
double kl_exact(const std::vector<double>& P, const std::vector<double>& Y, int n, int c,
                double dof, bool compute_error, std::vector<double>& grad) {
    const double expo = (dof + 1.0) / -2.0;
    std::vector<double> W(static_cast<std::size_t>(n) * n, 0.0);
    double sum_w = 0.0;                                   // over i < j, as pdist
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            double w = sq_dist(&Y[static_cast<std::size_t>(i) * c], &Y[static_cast<std::size_t>(j) * c], c);
            w = std::pow(w / dof + 1.0, expo);
            W[static_cast<std::size_t>(i) * n + j] = w;
            sum_w += w;
        }
    const double norm = 2.0 * sum_w;
    double kl = 0.0;
    grad.assign(static_cast<std::size_t>(n) * c, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const double w = W[static_cast<std::size_t>(i) * n + j];
            const double q = std::max(w / norm, kMachineEpsilon);
            const double p = P[static_cast<std::size_t>(i) * n + j];
            if (compute_error) kl += p * std::log(std::max(p, kMachineEpsilon) / q);
            const double pqd = (p - q) * w;
            for (int a = 0; a < c; ++a) {
                const double diff = Y[static_cast<std::size_t>(i) * c + a] - Y[static_cast<std::size_t>(j) * c + a];
                grad[static_cast<std::size_t>(i) * c + a] += pqd * diff;
                grad[static_cast<std::size_t>(j) * c + a] -= pqd * diff;
            }
        }
    const double scale = 2.0 * (dof + 1.0) / dof;
    for (double& g : grad) g *= scale;
    return 2.0 * kl;
}

/// A 2^c-ary space-partitioning tree (quadtree for c = 2, octree for c = 3)
/// holding centre of mass and count per cell, for the Barnes-Hut negative
/// forces.
class SpTree {
public:
    SpTree(const std::vector<double>& Y, int n, int c) : Y_(Y), c_(c), children_(1 << c) {
        std::vector<double> lo(c, std::numeric_limits<double>::infinity());
        std::vector<double> hi(c, -std::numeric_limits<double>::infinity());
        for (int i = 0; i < n; ++i)
            for (int a = 0; a < c; ++a) {
                lo[a] = std::min(lo[a], Y[static_cast<std::size_t>(i) * c + a]);
                hi[a] = std::max(hi[a], Y[static_cast<std::size_t>(i) * c + a]);
            }
        Node root;
        root.centre.resize(c);
        root.half.resize(c);
        for (int a = 0; a < c; ++a) {
            root.centre[a] = 0.5 * (lo[a] + hi[a]);
            root.half[a] = std::max(0.5 * (hi[a] - lo[a]), 1e-5) * (1.0 + 1e-3);
        }
        root.com.assign(c, 0.0);
        nodes_.push_back(root);
        for (int i = 0; i < n; ++i) insert(0, i, 0);
    }

    /// Accumulates sum_Q and the negative force on point `i` (sklearn
    /// compute_gradient_negative, van der Maaten's computeNonEdgeForces).
    void negative(int i, double theta2, double dof, double* neg, double& sum_q) const {
        visit(0, i, theta2, dof, neg, sum_q);
    }

private:
    struct Node {
        std::vector<double> centre, half, com;
        int count = 0;
        int first_child = -1;           // children are contiguous
        std::vector<int> points;        // leaf members
    };

    void insert(int node, int i, int depth) {
        const double* y = &Y_[static_cast<std::size_t>(i) * c_];
        Node& nd0 = nodes_[node];
        for (int a = 0; a < c_; ++a)
            nd0.com[a] = (nd0.com[a] * nd0.count + y[a]) / (nd0.count + 1);
        nd0.count++;
        if (nodes_[node].first_child < 0) {
            nodes_[node].points.push_back(i);
            if (nodes_[node].points.size() == 1 || depth >= 50) return;
            bool all_same = true;               // coincident points stay together
            for (int p : nodes_[node].points)
                for (int a = 0; a < c_; ++a)
                    if (Y_[static_cast<std::size_t>(p) * c_ + a] != y[a]) all_same = false;
            if (all_same) return;
            subdivide(node);
            std::vector<int> pts;
            pts.swap(nodes_[node].points);
            for (int p : pts) insert(child_for(node, p), p, depth + 1);
            return;
        }
        insert(child_for(node, i), i, depth + 1);
    }

    void subdivide(int node) {
        const int first = static_cast<int>(nodes_.size());
        for (int ch = 0; ch < children_; ++ch) {
            Node child;
            child.centre.resize(c_);
            child.half.resize(c_);
            child.com.assign(c_, 0.0);
            for (int a = 0; a < c_; ++a) {
                child.half[a] = 0.5 * nodes_[node].half[a];
                child.centre[a] = nodes_[node].centre[a] + ((ch >> a) & 1 ? child.half[a] : -child.half[a]);
            }
            nodes_.push_back(child);
        }
        nodes_[node].first_child = first;
    }

    int child_for(int node, int i) const {
        int ch = 0;
        for (int a = 0; a < c_; ++a)
            if (Y_[static_cast<std::size_t>(i) * c_ + a] > nodes_[node].centre[a]) ch |= (1 << a);
        return nodes_[node].first_child + ch;
    }

    void visit(int node, int i, double theta2, double dof, double* neg, double& sum_q) const {
        const Node& nd = nodes_[node];
        if (nd.count == 0) return;
        const bool leaf = nd.first_child < 0;
        if (leaf && nd.count == 1 && nd.points[0] == i) return;     // the point itself
        const double* y = &Y_[static_cast<std::size_t>(i) * c_];
        double d2 = 0.0, max_w2 = 0.0;
        for (int a = 0; a < c_; ++a) {
            const double diff = y[a] - nd.com[a];
            d2 += diff * diff;
            max_w2 = std::max(max_w2, 4.0 * nd.half[a] * nd.half[a]);
        }
        if (leaf || max_w2 < theta2 * d2) {
            // A leaf may hold the point itself among coincident duplicates.
            double size = nd.count;
            if (leaf) for (int p : nd.points) if (p == i) size -= 1.0;
            if (size <= 0) return;
            double q = dof / (dof + d2);
            if (dof != 1.0) q = std::pow(q, (dof + 1.0) / 2.0);
            sum_q += size * q;
            const double mult = size * q * q;
            for (int a = 0; a < c_; ++a) neg[a] += mult * (y[a] - nd.com[a]);
            return;
        }
        for (int ch = 0; ch < children_; ++ch) visit(nd.first_child + ch, i, theta2, dof, neg, sum_q);
    }

    const std::vector<double>& Y_;
    int c_, children_;
    std::vector<Node> nodes_;
};

/// sklearn _kl_divergence_bh: positive forces over the kNN graph, negative
/// forces through the tree.
double kl_bh(const Csr& P, const std::vector<double>& Y, int n, int c, double dof,
             double angle, bool compute_error, std::vector<double>& grad) {
    SpTree tree(Y, n, c);
    std::vector<double> neg(static_cast<std::size_t>(n) * c, 0.0);
    double sum_q = 0.0;
    for (int i = 0; i < n; ++i) tree.negative(i, angle * angle, dof, &neg[static_cast<std::size_t>(i) * c], sum_q);
    grad.assign(static_cast<std::size_t>(n) * c, 0.0);
    double kl = 0.0;
    const double expo = (dof + 1.0) / 2.0;
    for (int i = 0; i < n; ++i)
        for (int e = P.indptr[i]; e < P.indptr[i + 1]; ++e) {
            const int j = P.indices[e];
            const double p = P.data[e];
            double d2 = 0.0;
            for (int a = 0; a < c; ++a) {
                const double diff = Y[static_cast<std::size_t>(i) * c + a] - Y[static_cast<std::size_t>(j) * c + a];
                d2 += diff * diff;
            }
            double q = dof / (dof + d2);
            if (dof != 1.0) q = std::pow(q, expo);
            const double pq = p * q;
            for (int a = 0; a < c; ++a)
                grad[static_cast<std::size_t>(i) * c + a] +=
                    pq * (Y[static_cast<std::size_t>(i) * c + a] - Y[static_cast<std::size_t>(j) * c + a]);
            if (compute_error) {
                const double qn = q / sum_q;
                kl += p * std::log(std::max(p, kFloat32Tiny) / std::max(qn, kFloat32Tiny));
            }
        }
    const double scale = 2.0 * (dof + 1.0) / dof;
    for (std::size_t m = 0; m < grad.size(); ++m) grad[m] = scale * (grad[m] - neg[m] / sum_q);
    return kl;
}

/// sklearn _gradient_descent. Returns the last iteration index; `error` gets
/// the last computed KL divergence.
template <typename Objective>
int gradient_descent(Objective objective, std::vector<double>& p, int it, int max_iter,
                     int n_iter_without_progress, double momentum, double learning_rate,
                     double min_grad_norm, double& error) {
    const double min_gain = 0.01;
    std::vector<double> update(p.size(), 0.0), gains(p.size(), 1.0), grad;
    error = std::numeric_limits<double>::max();
    double best_error = std::numeric_limits<double>::max();
    int best_iter = it, i = it;
    for (i = it; i < max_iter; ++i) {
        const bool check = (i + 1) % kIterCheck == 0;
        const bool compute_error = check || i == max_iter - 1;
        const double e = objective(p, compute_error, grad);
        if (compute_error) error = e;
        for (std::size_t m = 0; m < p.size(); ++m) {
            if (update[m] * grad[m] < 0.0) gains[m] += 0.2;
            else gains[m] *= 0.8;
            gains[m] = std::max(gains[m], min_gain);
            grad[m] *= gains[m];
            update[m] = momentum * update[m] - learning_rate * grad[m];
            p[m] += update[m];
        }
        if (check) {
            double grad_norm = 0.0;
            for (double g : grad) grad_norm += g * g;
            grad_norm = std::sqrt(grad_norm);
            if (error < best_error) {
                best_error = error;
                best_iter = i;
            } else if (i - best_iter > n_iter_without_progress) {
                break;
            }
            if (grad_norm <= min_grad_norm) break;
        }
    }
    return std::min(i, max_iter - 1);
}

}  // namespace

void tsne_joint_probabilities(const double* data, int n_samples, int n_features,
                              double perplexity,
                              double** out_P, int* n_rows, int* n_cols) {
    check_data(data, n_samples, n_features, "tsne_joint_probabilities");
    if (!(perplexity > 0)) throw std::invalid_argument("tsne_joint_probabilities: perplexity must be > 0");
    std::vector<double> P = joint_p_exact(data, n_samples, n_features, perplexity);
    *out_P = to_new_buffer<double>(P);
    *n_rows = n_samples;
    *n_cols = n_samples;
}

void tsne_joint_probabilities_nn(const double* data, int n_samples, int n_features,
                                 double perplexity,
                                 long long** out_row, int* n_out_row,
                                 long long** out_col, int* n_out_col,
                                 double** out_value, int* n_out_value) {
    check_data(data, n_samples, n_features, "tsne_joint_probabilities_nn");
    if (!(perplexity > 0)) throw std::invalid_argument("tsne_joint_probabilities_nn: perplexity must be > 0");
    Csr P = joint_p_nn(data, n_samples, n_features, perplexity);
    std::vector<long long> rows;
    rows.reserve(P.data.size());
    for (int i = 0; i < n_samples; ++i)
        for (int e = P.indptr[i]; e < P.indptr[i + 1]; ++e) rows.push_back(i);
    *out_row = to_new_buffer<long long>(rows);
    *n_out_row = static_cast<int>(rows.size());
    *out_col = to_new_buffer<long long>(P.indices);
    *n_out_col = static_cast<int>(P.indices.size());
    *out_value = to_new_buffer<double>(P.data);
    *n_out_value = static_cast<int>(P.data.size());
}

void tsne_embed(const double* data, int n_samples, int n_features,
                const double* init, int init_rows, int init_cols,
                int n_components, double perplexity, double early_exaggeration,
                double learning_rate, int max_iter, int n_iter_without_progress,
                double min_grad_norm, int method, double angle,
                double** out_embedding, int* n_out_rows, int* n_out_cols,
                double* kl_divergence, int* n_iter) {
    check_data(data, n_samples, n_features, "tsne_embed");
    const int n = n_samples, c = n_components;
    if (c < 1) throw std::invalid_argument("tsne_embed: n_components must be >= 1");
    if (!(perplexity > 0)) throw std::invalid_argument("tsne_embed: perplexity must be > 0");
    if (perplexity >= n) throw std::invalid_argument("tsne_embed: perplexity must be less than n_samples");
    if (!(early_exaggeration >= 1.0)) throw std::invalid_argument("tsne_embed: early_exaggeration must be >= 1");
    if (max_iter < kExplorationIter) throw std::invalid_argument("tsne_embed: max_iter must be >= 250");
    if (method != 0 && method != 1) throw std::invalid_argument("tsne_embed: method is 0 (exact) or 1 (Barnes-Hut)");
    if (method == 1 && (c < 2 || c > 3))
        throw std::invalid_argument("tsne_embed: Barnes-Hut needs n_components 2 or 3; use the exact method");
    if (method == 1 && !(angle >= 0.0 && angle <= 1.0))
        throw std::invalid_argument("tsne_embed: angle must be in [0, 1]");

    std::vector<double> Y;
    if (init != nullptr && init_rows > 0) {
        if (init_rows != n || init_cols != c)
            throw std::invalid_argument("tsne_embed: init must be n_samples x n_components");
        Y.assign(init, init + static_cast<std::size_t>(n) * c);
    } else {
        // sklearn: PCA, cast to float32, scaled to a first-axis std of 1e-4.
        Y = embedding_detail::pca_project(data, n, n_features, c);
        for (double& v : Y) v = static_cast<double>(static_cast<float>(v));
        double m = 0.0, s = 0.0;
        for (int i = 0; i < n; ++i) m += Y[static_cast<std::size_t>(i) * c];
        m /= n;
        for (int i = 0; i < n; ++i) {
            const double dv = Y[static_cast<std::size_t>(i) * c] - m;
            s += dv * dv;
        }
        s = std::sqrt(s / n);
        if (s > 0) for (double& v : Y) v = v / s * 1e-4;
    }

    const double lr = learning_rate > 0 ? learning_rate : std::max(n / early_exaggeration / 4.0, 50.0);
    const double dof = std::max(c - 1, 1);
    double error = 0.0;
    int it = 0;
    if (method == 0) {
        std::vector<double> P = joint_p_exact(data, n, n_features, perplexity);
        auto obj = [&](const std::vector<double>& p, bool ce, std::vector<double>& g) {
            return kl_exact(P, p, n, c, dof, ce, g);
        };
        for (double& v : P) v *= early_exaggeration;
        it = gradient_descent(obj, Y, 0, kExplorationIter, kExplorationIter, 0.5, lr, min_grad_norm, error);
        for (double& v : P) v /= early_exaggeration;
        if (it < kExplorationIter || max_iter - kExplorationIter > 0)
            it = gradient_descent(obj, Y, it + 1, max_iter, n_iter_without_progress, 0.8, lr, min_grad_norm, error);
    } else {
        Csr P = joint_p_nn(data, n, n_features, perplexity);
        auto obj = [&](const std::vector<double>& p, bool ce, std::vector<double>& g) {
            return kl_bh(P, p, n, c, dof, angle, ce, g);
        };
        for (double& v : P.data) v *= early_exaggeration;
        it = gradient_descent(obj, Y, 0, kExplorationIter, kExplorationIter, 0.5, lr, min_grad_norm, error);
        for (double& v : P.data) v /= early_exaggeration;
        if (it < kExplorationIter || max_iter - kExplorationIter > 0)
            it = gradient_descent(obj, Y, it + 1, max_iter, n_iter_without_progress, 0.8, lr, min_grad_norm, error);
    }
    *out_embedding = to_new_buffer<double>(Y);
    *n_out_rows = n;
    *n_out_cols = c;
    *kl_divergence = error;
    *n_iter = it;
}

}  // namespace tttrlib
