// SPDX-License-Identifier: BSD-3-Clause
// UMAP following umap-learn 0.5 (umap_.py: smooth_knn_dist,
// compute_membership_strengths, fuzzy_simplicial_set, find_ab_params,
// make_epochs_per_sample, simplicial_set_embedding; spectral.py:
// spectral_layout; layouts.py: optimize_layout_euclidean) step for step,
// including its float32 storage of rho, sigma and the memberships. See
// Embedding.h for where it differs (random streams, the eigensolver).

#include "Embedding.h"
#include "EmbeddingDetail.h"
#include "SimPcgRandom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tttrlib {
namespace {

using embedding_detail::check_data;
using embedding_detail::to_new_buffer;

struct Coo {
    std::vector<int> row, col;
    std::vector<double> val;
};

/// umap smooth_knn_dist (n_iter 64, bandwidth 1). `dist` is n x k with the
/// point itself in column 0; rho and sigma are stored as float32, as there.
void smooth_knn_dist(const std::vector<double>& dist, int n, int k, double local_connectivity,
                     std::vector<double>& sigma, std::vector<double>& rho) {
    const double tol = 1e-5, min_k_dist_scale = 1e-3;
    const double target = std::log2(static_cast<double>(k));
    double mean_all = 0.0;
    for (double v : dist) mean_all += v;
    mean_all /= static_cast<double>(dist.size());
    sigma.assign(n, 0.0);
    rho.assign(n, 0.0);
    std::vector<double> nz;
    for (int i = 0; i < n; ++i) {
        const double* di = &dist[static_cast<std::size_t>(i) * k];
        nz.clear();
        for (int j = 0; j < k; ++j) if (di[j] > 0.0) nz.push_back(di[j]);
        if (static_cast<double>(nz.size()) >= local_connectivity) {
            const int index = static_cast<int>(std::floor(local_connectivity));
            const double interpolation = local_connectivity - index;
            if (index > 0) {
                rho[i] = nz[index - 1];
                if (interpolation > tol) rho[i] += interpolation * (nz[index] - nz[index - 1]);
            } else {
                rho[i] = interpolation * nz[0];
            }
        } else if (!nz.empty()) {
            rho[i] = *std::max_element(nz.begin(), nz.end());
        }
        rho[i] = static_cast<float>(rho[i]);
        double lo = 0.0, hi = std::numeric_limits<double>::infinity(), mid = 1.0;
        for (int it = 0; it < 64; ++it) {
            double psum = 0.0;
            for (int j = 1; j < k; ++j) {
                const double d = di[j] - rho[i];
                psum += d > 0.0 ? std::exp(-(d / mid)) : 1.0;
            }
            if (std::fabs(psum - target) < tol) break;
            if (psum > target) {
                hi = mid;
                mid = (lo + hi) / 2.0;
            } else {
                lo = mid;
                mid = std::isinf(hi) ? mid * 2.0 : (lo + hi) / 2.0;
            }
        }
        sigma[i] = mid;
        if (rho[i] > 0.0) {
            double mean_i = 0.0;
            for (int j = 0; j < k; ++j) mean_i += di[j];
            mean_i /= k;
            if (sigma[i] < min_k_dist_scale * mean_i) sigma[i] = min_k_dist_scale * mean_i;
        } else if (sigma[i] < min_k_dist_scale * mean_all) {
            sigma[i] = min_k_dist_scale * mean_all;
        }
        sigma[i] = static_cast<float>(sigma[i]);
    }
}

/// Membership strengths and the fuzzy set operation: the symmetric UMAP
/// graph, entries sorted by (row, col), zeros dropped.
Coo fuzzy_graph(const double* X, int n, int d, int n_neighbors, double local_connectivity,
                double set_op_mix_ratio) {
    std::vector<int> idx;
    std::vector<double> dist;
    embedding_detail::knn(X, n, d, n_neighbors, idx, dist);
    for (double& v : dist) v = static_cast<float>(v);     // umap: knn_dists.astype(float32)
    std::vector<double> sigma, rho;
    smooth_knn_dist(dist, n, n_neighbors, local_connectivity, sigma, rho);

    // W as per-row maps; W^T by scattering.
    std::vector<std::vector<std::pair<int, double>>> w(n), wt(n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n_neighbors; ++j) {
            const int c = idx[static_cast<std::size_t>(i) * n_neighbors + j];
            const double dij = dist[static_cast<std::size_t>(i) * n_neighbors + j];
            double v;
            if (c == i) v = 0.0;
            else if (dij - rho[i] <= 0.0 || sigma[i] == 0.0) v = 1.0;
            else v = std::exp(-((dij - rho[i]) / sigma[i]));
            v = static_cast<float>(v);
            if (v == 0.0) continue;
            w[i].emplace_back(c, v);
            wt[c].emplace_back(i, v);
        }
    Coo g;
    std::vector<std::pair<int, double>> a, b;
    for (int i = 0; i < n; ++i) {
        a = w[i];
        b = wt[i];
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        std::size_t p = 0, q = 0;
        while (p < a.size() || q < b.size()) {
            int col;
            double x = 0.0, y = 0.0;
            if (q >= b.size() || (p < a.size() && a[p].first < b[q].first)) {
                col = a[p].first, x = a[p++].second;
            } else if (p >= a.size() || b[q].first < a[p].first) {
                col = b[q].first, y = b[q++].second;
            } else {
                col = a[p].first, x = a[p++].second, y = b[q++].second;
            }
            const double prod = x * y;
            const double v = set_op_mix_ratio * (x + y - prod) + (1.0 - set_op_mix_ratio) * prod;
            if (v != 0.0) {
                g.row.push_back(i);
                g.col.push_back(col);
                g.val.push_back(v);
            }
        }
    }
    return g;
}

/// find_ab_params: least squares of 1 / (1 + a x^(2b)) to umap's target curve
/// on 300 points, by Levenberg-Marquardt from (1, 1) as scipy curve_fit.
void find_ab(double spread, double min_dist, double& a_out, double& b_out) {
    const int m = 300;
    std::vector<double> x(m), y(m);
    for (int i = 0; i < m; ++i) {
        x[i] = spread * 3.0 * i / (m - 1);
        y[i] = x[i] < min_dist ? 1.0 : std::exp(-(x[i] - min_dist) / spread);
    }
    auto sse = [&](double a, double b) {
        double s = 0.0;
        for (int i = 0; i < m; ++i) {
            const double f = 1.0 / (1.0 + a * std::pow(x[i], 2.0 * b));
            s += (f - y[i]) * (f - y[i]);
        }
        return s;
    };
    double a = 1.0, b = 1.0, lambda = 1e-3, cost = sse(a, b);
    for (int it = 0; it < 2000; ++it) {
        double jtj00 = 0, jtj01 = 0, jtj11 = 0, jtr0 = 0, jtr1 = 0;
        for (int i = 0; i < m; ++i) {
            const double xb = x[i] > 0 ? std::pow(x[i], 2.0 * b) : 0.0;
            const double den = 1.0 + a * xb;
            const double f = 1.0 / den;
            const double r = f - y[i];
            const double da = -xb / (den * den);
            const double db = x[i] > 0 ? -a * xb * 2.0 * std::log(x[i]) / (den * den) : 0.0;
            jtj00 += da * da, jtj01 += da * db, jtj11 += db * db;
            jtr0 += da * r, jtr1 += db * r;
        }
        bool improved = false;
        for (int tries = 0; tries < 50 && !improved; ++tries) {
            const double m00 = jtj00 * (1 + lambda), m11 = jtj11 * (1 + lambda);
            const double det = m00 * m11 - jtj01 * jtj01;
            if (det == 0) break;
            const double sa = -(m11 * jtr0 - jtj01 * jtr1) / det;
            const double sb = -(m00 * jtr1 - jtj01 * jtr0) / det;
            const double c = sse(a + sa, b + sb);
            if (c < cost) {
                const double rel = (cost - c) / std::max(cost, 1e-300);
                a += sa, b += sb, cost = c, lambda *= 0.3, improved = true;
                if (rel < 1e-15 && std::fabs(sa) < 1e-12 && std::fabs(sb) < 1e-12) it = 2000;
            } else {
                lambda *= 10.0;
            }
        }
        if (!improved) break;
    }
    a_out = a;
    b_out = b;
}

double normal(SimPcgRandom& rng) {                 // Box-Muller
    const double u1 = rng.random0e1e(), u2 = rng.random0e1e();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

/// The top `c` non-trivial eigenvectors of D^-1/2 W D^-1/2 (umap
/// spectral_layout's eigenvectors of the normalised Laplacian, skipping the
/// first), by thick-restart Lanczos: every cycle is a Rayleigh-Ritz step on
/// [kept Ritz vectors, the Krylov continuation, new Lanczos vectors], fully
/// reorthogonalised and deflated against the trivial eigenvector. Keeping the
/// wanted Ritz vectors across restarts is what resolves the clusters of
/// eigenvalues near 1 that long, path-like graphs have (a single-vector
/// restart does not). Converged: every wanted residual below `tol`; after
/// `max_cycles` the best estimate is accepted if its residuals are below 1e-4
/// (ARPACK's tolerance in umap-learn), otherwise false.
bool spectral_init(const Coo& g, int n, int c, SimPcgRandom& rng, std::vector<double>& out,
                   double tol = 1e-8, int max_cycles = 2000) {
    if (n <= c + 1) return false;
    std::vector<double> deg(n, 0.0);
    for (std::size_t e = 0; e < g.val.size(); ++e) deg[g.row[e]] += g.val[e];
    std::vector<double> isd(n);
    for (int i = 0; i < n; ++i) {
        if (deg[i] <= 0) return false;
        isd[i] = 1.0 / std::sqrt(deg[i]);
    }
    std::vector<double> v0(n);                           // the trivial eigenvector
    double nv0 = 0.0;
    for (int i = 0; i < n; ++i) v0[i] = std::sqrt(deg[i]), nv0 += deg[i];
    nv0 = std::sqrt(nv0);
    for (double& v : v0) v /= nv0;
    using Vec = std::vector<double>;
    auto apply = [&](const Vec& x, Vec& y) {
        y.assign(n, 0.0);
        for (std::size_t e = 0; e < g.val.size(); ++e)
            y[g.row[e]] += g.val[e] * isd[g.row[e]] * isd[g.col[e]] * x[g.col[e]];
    };
    auto dot = [&](const Vec& p, const Vec& q) {
        double s = 0.0;
        for (int i = 0; i < n; ++i) s += p[i] * q[i];
        return s;
    };
    auto axpy = [&](double alpha, const Vec& p, Vec& q) {
        for (int i = 0; i < n; ++i) q[i] += alpha * p[i];
    };
    // Orthogonalise w against v0 and V (twice); returns the remaining norm.
    auto orth = [&](Vec& w, const std::vector<Vec>& V) {
        for (int pass = 0; pass < 2; ++pass) {
            axpy(-dot(v0, w), v0, w);
            for (const Vec& q : V) axpy(-dot(q, w), q, w);
        }
        return std::sqrt(dot(w, w));
    };

    const int dim = n - 1;                               // v0's complement
    const int keep = std::min(c + 8, dim);
    // A basis well beyond the wanted count: the eigenvalues next to 1 sit
    // ~1e-4 apart on path-like graphs, and a Krylov space resolves such a
    // cluster only once it is several times larger than the cluster.
    const int m = std::min(dim, std::max(keep + 24, 128));
    std::vector<Vec> V, AV;                              // basis and its image
    Vec f(n);                                            // Krylov continuation
    for (double& v : f) v = normal(rng);
    if (orth(f, V) == 0) return false;
    std::vector<double> best;
    double last_worst = std::numeric_limits<double>::infinity();
    for (int cycle = 0; cycle < max_cycles; ++cycle) {
        // expand: continue the Krylov sequence from f until the basis has m vectors
        while (static_cast<int>(V.size()) < m) {
            const double nf = orth(f, V);
            if (nf < 1e-12) {                             // invariant subspace: new random direction
                for (double& v : f) v = normal(rng);
                if (orth(f, V) < 1e-12) break;
                continue;
            }
            for (double& v : f) v /= nf;
            V.push_back(f);
            Vec w;
            apply(V.back(), w);
            AV.push_back(w);
            f = w;
        }
        // The continuation must be orthogonal to the FULL basis before it is
        // rotated and truncated -- that makes it the next Lanczos vector, to
        // which every Ritz residual is parallel. Orthogonalised against the
        // kept vectors only, it drags the discarded directions back in and
        // the restart stalls (measured: residual stuck at 1e-3 after 300
        // cycles, against 1e-11 after 2 with this line).
        orth(f, V);
        const int k = static_cast<int>(V.size());
        std::vector<double> H(static_cast<std::size_t>(k) * k);
        for (int a = 0; a < k; ++a)
            for (int b = a; b < k; ++b) H[a * k + b] = H[b * k + a] = 0.5 * (dot(V[a], AV[b]) + dot(V[b], AV[a]));
        std::vector<double> evals, evecs;
        embedding_detail::sym_eig(H, k, evals, evecs);
        // Ritz vectors, their images, and the residuals of the wanted ones.
        const int kk = std::min(keep, k);
        std::vector<Vec> X(kk, Vec(n, 0.0)), AX(kk, Vec(n, 0.0));
        for (int t = 0; t < kk; ++t)
            for (int j = 0; j < k; ++j) {
                const double s = evecs[j * k + t];
                axpy(s, V[j], X[t]);
                axpy(s, AV[j], AX[t]);
            }
        double worst = 0.0;
        for (int t = 0; t < std::min(c, kk); ++t) {
            Vec r = AX[t];
            axpy(-evals[t], X[t], r);
            worst = std::max(worst, std::sqrt(dot(r, r)));
        }
        best.assign(static_cast<std::size_t>(n) * c, 0.0);
        for (int t = 0; t < std::min(c, kk); ++t)
            for (int i = 0; i < n; ++i) best[static_cast<std::size_t>(i) * c + t] = X[t][i];
        if (worst <= tol && kk >= c) {
            out = best;
            return true;
        }
        // thick restart: keep the Ritz vectors, continue from f.
        V.swap(X);
        AV.swap(AX);
        last_worst = worst;
    }
    out = best;
    return last_worst <= 1e-4;          // ARPACK's tolerance in umap-learn
}

inline double clip4(double v) { return v > 4.0 ? 4.0 : (v < -4.0 ? -4.0 : v); }

}  // namespace

void umap_spectral_layout(const long long* row, int n_row, const long long* col, int n_col,
                          const double* value, int n_value, int n_samples, int n_components,
                          int seed, double** out_layout, int* n_out_rows, int* n_out_cols) {
    if (n_row != n_col || n_row != n_value)
        throw std::invalid_argument("umap_spectral_layout: row, col and value must have one length");
    if (n_samples < 2 || n_components < 1)
        throw std::invalid_argument("umap_spectral_layout: need n_samples >= 2, n_components >= 1");
    if (seed < 0) throw std::invalid_argument("umap_spectral_layout: seed must be >= 0");
    Coo g;
    for (int e = 0; e < n_row; ++e) {
        if (row[e] < 0 || row[e] >= n_samples || col[e] < 0 || col[e] >= n_samples)
            throw std::invalid_argument("umap_spectral_layout: index out of range");
        g.row.push_back(static_cast<int>(row[e]));
        g.col.push_back(static_cast<int>(col[e]));
        g.val.push_back(value[e]);
    }
    SimPcgRandom rng;
    rng.reset(static_cast<uint32_t>(seed), 0x5eed);
    std::vector<double> layout;
    if (!spectral_init(g, n_samples, n_components, rng, layout))
        throw std::runtime_error("umap_spectral_layout: the eigensolver did not converge "
                                 "(disconnected or degenerate graph)");
    *out_layout = to_new_buffer<double>(layout);
    *n_out_rows = n_samples;
    *n_out_cols = n_components;
}

void umap_find_ab_params(double spread, double min_dist, double* a, double* b) {
    if (!(spread > 0)) throw std::invalid_argument("umap_find_ab_params: spread must be > 0");
    if (!(min_dist >= 0) || min_dist > spread)
        throw std::invalid_argument("umap_find_ab_params: need 0 <= min_dist <= spread");
    find_ab(spread, min_dist, *a, *b);
}

void umap_fuzzy_graph(const double* data, int n_samples, int n_features,
                      int n_neighbors, double local_connectivity,
                      double set_op_mix_ratio,
                      long long** out_row, int* n_out_row,
                      long long** out_col, int* n_out_col,
                      double** out_value, int* n_out_value) {
    check_data(data, n_samples, n_features, "umap_fuzzy_graph");
    if (n_neighbors < 2 || n_neighbors > n_samples)
        throw std::invalid_argument("umap_fuzzy_graph: need 2 <= n_neighbors <= n_samples");
    Coo g = fuzzy_graph(data, n_samples, n_features, n_neighbors, local_connectivity, set_op_mix_ratio);
    *out_row = to_new_buffer<long long>(g.row);
    *n_out_row = static_cast<int>(g.row.size());
    *out_col = to_new_buffer<long long>(g.col);
    *n_out_col = static_cast<int>(g.col.size());
    *out_value = to_new_buffer<double>(g.val);
    *n_out_value = static_cast<int>(g.val.size());
}

void umap_embed(const double* data, int n_samples, int n_features,
                const double* init, int init_rows, int init_cols,
                int n_components, int n_neighbors, double min_dist, double spread,
                int n_epochs, double learning_rate, double negative_sample_rate,
                double repulsion_strength, double local_connectivity,
                double set_op_mix_ratio, double a, double b, int seed,
                double** out_embedding, int* n_out_rows, int* n_out_cols) {
    check_data(data, n_samples, n_features, "umap_embed");
    const int n = n_samples, c = n_components;
    if (c < 1) throw std::invalid_argument("umap_embed: n_components must be >= 1");
    if (n_neighbors < 2 || n_neighbors > n)
        throw std::invalid_argument("umap_embed: need 2 <= n_neighbors <= n_samples");
    if (!(learning_rate > 0)) throw std::invalid_argument("umap_embed: learning_rate must be > 0");
    if (!(negative_sample_rate >= 0)) throw std::invalid_argument("umap_embed: negative_sample_rate must be >= 0");
    if (!(a > 0) || !(b > 0)) umap_find_ab_params(spread, min_dist, &a, &b);

    if (seed < 0) throw std::invalid_argument("umap_embed: seed must be >= 0");
    SimPcgRandom rng;
    rng.reset(static_cast<uint32_t>(seed), 0x5eed);

    Coo g = fuzzy_graph(data, n, n_features, n_neighbors, local_connectivity, set_op_mix_ratio);
    const int default_epochs = n <= 10000 ? 500 : 200;
    const int epochs = n_epochs > 0 ? n_epochs : default_epochs;
    double wmax = 0.0;
    for (double v : g.val) wmax = std::max(wmax, v);
    const double cut = wmax / static_cast<double>(epochs > 10 ? epochs : default_epochs);
    Coo pruned;
    for (std::size_t e = 0; e < g.val.size(); ++e)
        if (!(g.val[e] < cut)) {
            pruned.row.push_back(g.row[e]);
            pruned.col.push_back(g.col[e]);
            pruned.val.push_back(g.val[e]);
        }

    std::vector<double> Y(static_cast<std::size_t>(n) * c);
    if (init != nullptr && init_rows > 0) {
        if (init_rows != n || init_cols != c)
            throw std::invalid_argument("umap_embed: init must be n_samples x n_components");
        Y.assign(init, init + static_cast<std::size_t>(n) * c);
    } else {
        std::vector<double> spec;
        if (spectral_init(pruned, n, c, rng, spec)) {
            double mx = 0.0;
            for (double v : spec) mx = std::max(mx, std::fabs(v));
            const double expansion = mx > 0 ? 10.0 / mx : 1.0;
            for (std::size_t m = 0; m < Y.size(); ++m)
                Y[m] = static_cast<float>(spec[m] * expansion) + static_cast<float>(1e-4 * normal(rng));
        } else {                                     // umap's fallback: uniform in [-10, 10]
            for (double& v : Y) v = -10.0 + 20.0 * rng.random0i1e();
        }
    }
    for (int a_ = 0; a_ < c; ++a_) {                  // rescale every axis to [0, 10]
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (int i = 0; i < n; ++i) {
            lo = std::min(lo, Y[static_cast<std::size_t>(i) * c + a_]);
            hi = std::max(hi, Y[static_cast<std::size_t>(i) * c + a_]);
        }
        const double span = hi > lo ? hi - lo : 1.0;
        for (int i = 0; i < n; ++i)
            Y[static_cast<std::size_t>(i) * c + a_] = 10.0 * (Y[static_cast<std::size_t>(i) * c + a_] - lo) / span;
    }

    // make_epochs_per_sample and optimize_layout_euclidean.
    const std::size_t ne = pruned.val.size();
    double pmax = 0.0;
    for (double v : pruned.val) pmax = std::max(pmax, v);
    std::vector<double> eps(ne, -1.0), epns(ne), next(ne), next_neg(ne);
    for (std::size_t e = 0; e < ne; ++e) {
        const double ns = epochs * (pruned.val[e] / pmax);
        if (ns > 0) eps[e] = epochs / ns;
        epns[e] = eps[e] / negative_sample_rate;
        next[e] = eps[e];
        next_neg[e] = epns[e];
    }
    const double gamma = repulsion_strength;
    double alpha = learning_rate;
    for (int ep = 0; ep < epochs; ++ep) {
        for (std::size_t e = 0; e < ne; ++e) {
            if (!(next[e] <= ep)) continue;
            const int j = pruned.row[e], k = pruned.col[e];
            double* cur = &Y[static_cast<std::size_t>(j) * c];
            double* oth = &Y[static_cast<std::size_t>(k) * c];
            double d2 = 0.0;
            for (int t = 0; t < c; ++t) d2 += (cur[t] - oth[t]) * (cur[t] - oth[t]);
            double gc = 0.0;
            if (d2 > 0.0) gc = -2.0 * a * b * std::pow(d2, b - 1.0) / (a * std::pow(d2, b) + 1.0);
            for (int t = 0; t < c; ++t) {
                const double gd = clip4(gc * (cur[t] - oth[t]));
                cur[t] += gd * alpha;
                oth[t] -= gd * alpha;
            }
            next[e] += eps[e];
            const int n_neg = negative_sample_rate > 0
                ? static_cast<int>((ep - next_neg[e]) / epns[e]) : 0;
            for (int p = 0; p < n_neg; ++p) {
                const int kk = static_cast<int>(rng.next32() % static_cast<uint32_t>(n));
                double* o2 = &Y[static_cast<std::size_t>(kk) * c];
                double e2 = 0.0;
                for (int t = 0; t < c; ++t) e2 += (cur[t] - o2[t]) * (cur[t] - o2[t]);
                double rc;
                if (e2 > 0.0) rc = 2.0 * gamma * b / ((0.001 + e2) * (a * std::pow(e2, b) + 1.0));
                else if (j == kk) continue;
                else rc = 0.0;
                for (int t = 0; t < c; ++t) {
                    const double gd = rc > 0.0 ? clip4(rc * (cur[t] - o2[t])) : 0.0;
                    cur[t] += gd * alpha;
                }
            }
            if (n_neg > 0) next_neg[e] += n_neg * epns[e];
        }
        alpha = learning_rate * (1.0 - static_cast<double>(ep) / epochs);
    }
    *out_embedding = to_new_buffer<double>(Y);
    *n_out_rows = n;
    *n_out_cols = c;
}

}  // namespace tttrlib
