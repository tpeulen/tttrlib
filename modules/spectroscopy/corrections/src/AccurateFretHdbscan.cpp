// SPDX-License-Identifier: BSD-3-Clause
// Density-based population gating: HDBSCAN from modules/math over the scaled
// declared dimensions, clusters described afterwards by diagonal Gaussians for
// the assignment probabilities. See classify_populations_hdbscan in
// AccurateFretMultiDim.h for the choices and why.
#include "AccurateFretMultiDim.h"
#include "AccurateFretDetail.h"
#include "Cluster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace tttrlib {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kLog2Pi = std::log(2.0 * M_PI);

// owner of a buffer the Cluster.h kernels malloc
template <class T>
struct Buf {
    T* p = nullptr;
    int n = 0;
    Buf() = default;
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
    ~Buf() { std::free(p); }
};

// HDBSCAN over m sampled rows: a cluster id per row (-1 noise), the membership
// strength, and each cluster's birth distance (1 / lambda where it split off)
struct Density {
    std::vector<int> label;
    std::vector<double> strength;
    std::vector<double> birth;
};

Density run_hdbscan(const KDTree& tree, const std::vector<double>& core, int m, int mcs,
                    const std::string& selection, double epsilon) {
    const std::vector<double> mst = tree.mutual_reachability_mst(core, 1.0);
    const int e = m - 1;
    std::vector<long long> src(e), dst(e);
    std::vector<double> w(e);
    for (int i = 0; i < e; ++i) {
        src[i] = static_cast<long long>(mst[3 * i]);
        dst[i] = static_cast<long long>(mst[3 * i + 1]);
        w[i] = mst[3 * i + 2];
    }
    Buf<long long> par, chi, siz, roots;
    Buf<double> lam, str;
    Buf<unsigned char> sel;
    hdbscan_condensed_tree(src.data(), e, dst.data(), e, w.data(), e, mcs, &par.p, &par.n, &chi.p,
                           &chi.n, &lam.p, &lam.n, &siz.p, &siz.n);
    hdbscan_select_clusters(par.p, par.n, chi.p, chi.n, lam.p, lam.n, siz.p, siz.n, selection.c_str(),
                            false, epsilon, 0, &sel.p, &sel.n);
    hdbscan_label_points(par.p, par.n, chi.p, chi.n, sel.p, sel.n, m, &roots.p, &roots.n);
    hdbscan_membership_strengths(par.p, par.n, chi.p, chi.n, lam.p, lam.n, roots.p, roots.n, &str.p,
                                 &str.n);
    Density out;
    out.label.assign(m, -1);
    out.strength.assign(m, 0.0);
    std::map<long long, int> id;
    for (int i = 0; i < m; ++i) {
        if (roots.p[i] == m) continue;  // still rooted at the root: claimed by no cluster
        auto it = id.emplace(roots.p[i], static_cast<int>(id.size())).first;
        out.label[i] = it->second;
        out.strength[i] = str.p[i];
    }
    out.birth.assign(id.size(), std::numeric_limits<double>::infinity());
    for (int r = 0; r < chi.n; ++r) {
        auto it = id.find(chi.p[r]);
        if (it != id.end() && lam.p[r] > 0) out.birth[it->second] = 1.0 / lam.p[r];
    }
    return out;
}

} // namespace

PopulationSplit classify_populations_hdbscan(const std::vector<double>& x, int n_rows,
                                             const std::vector<std::string>& names,
                                             double donor_only_above, double acceptor_only_below,
                                             int min_population, double min_probability,
                                             double min_cluster_fraction, int min_cluster_size,
                                             int min_samples, int max_points,
                                             const std::string& selection, double epsilon,
                                             const std::vector<double>& width_floor) {
    const int d = static_cast<int>(names.size());
    const size_t n = static_cast<size_t>(n_rows);
    if (d == 0 || x.size() != n * d)
        throw std::invalid_argument("classify_populations_hdbscan: x must be (n_rows, len(names))");
    if (!width_floor.empty() && width_floor.size() != n * d)
        throw std::invalid_argument("classify_populations_hdbscan: width_floor must be empty or (n_rows, len(names))");
    if (selection != "leaf" && selection != "eom")
        throw std::invalid_argument("classify_populations_hdbscan: selection must be 'leaf' or 'eom'");

    // robust centre and width per dimension, a sentinel for missing values
    std::vector<double> med(d), wid(d), sentinel(d), lo(d), hi(d);
    for (int j = 0; j < d; ++j) {
        std::vector<double> v;
        for (size_t b = 0; b < n; ++b)
            if (std::isfinite(x[b * d + j])) v.push_back(x[b * d + j]);
        if (v.empty()) v.push_back(0.0);
        const std::vector<double> q = afret_detail::np_quantile(v, {0.25, 0.5, 0.75});
        double w = (q[2] - q[0]) / 1.349;
        if (!(w > 0)) w = afret_detail::np_std(v);
        if (!(w > 0)) w = 1.0;
        med[j] = q[1], wid[j] = w;
        lo[j] = *std::min_element(v.begin(), v.end());
        hi[j] = *std::max_element(v.begin(), v.end());
        sentinel[j] = (lo[j] - med[j]) / w - 3.0;
    }
    std::vector<size_t> rows;
    std::vector<double> z(n * d);
    for (size_t b = 0; b < n; ++b) {
        bool any = false;
        for (int j = 0; j < d; ++j) {
            const double v = x[b * d + j];
            any = any || std::isfinite(v);
            z[b * d + j] = std::isfinite(v) ? (v - med[j]) / wid[j] : sentinel[j];
        }
        if (any) rows.push_back(b);
    }

    // an even stride through the usable rows
    const size_t stride = max_points > 0 ? std::max<size_t>(1, (rows.size() + max_points - 1) / max_points) : 1;
    std::vector<size_t> sample;
    for (size_t i = 0; i < rows.size(); i += stride) sample.push_back(rows[i]);
    const int m = static_cast<int>(sample.size());
    int mcs = min_cluster_size > 0 ? min_cluster_size
                                   : std::max(10, static_cast<int>(std::lround(min_cluster_fraction * m)));
    int ms = min_samples > 0 ? min_samples : mcs;
    mcs = std::max(2, std::min(mcs, m / 2));
    ms = std::max(1, std::min(ms, m - 1));
    auto fallback = [&]() {
        return classify_populations_nd(x, n_rows, names, donor_only_above, acceptor_only_below, 6,
                                       min_population, min_probability);
    };
    if (m < 4) return fallback();

    std::vector<double> zs(static_cast<size_t>(m) * d);
    for (int i = 0; i < m; ++i)
        std::copy_n(&z[sample[i] * d], d, &zs[static_cast<size_t>(i) * d]);
    const KDTree tree(zs.data(), m, d, KDTree::default_leaf_size(d));
    const std::vector<double> core = tree.core_distances(ms);
    // splits closer than `epsilon` median core distances are not taken: shot
    // noise splits a population where it is dense, species part where it is sparse
    std::vector<double> sorted_core(core);
    std::nth_element(sorted_core.begin(), sorted_core.begin() + m / 2, sorted_core.end());
    const double eps = epsilon > 0 ? epsilon * sorted_core[m / 2] : 0.0;
    const Density dens = run_hdbscan(tree, core, m, mcs, selection, eps);
    const int k0 = static_cast<int>(dens.birth.size());
    if (k0 == 0) return fallback();

    // every usable row: its own label when sampled, else that of the sampled
    // neighbour it reaches first, if within the cluster's birth distance
    std::vector<int> label(n, -1);
    std::vector<double> membership(n, 0.0);
    std::vector<int> in_sample(n, -1);
    for (int i = 0; i < m; ++i) in_sample[sample[i]] = i;
    const int kq = std::min(10, m);
    std::vector<int> idx(kq);
    std::vector<double> sq(kq);
    for (size_t b : rows) {
        if (in_sample[b] >= 0) {
            label[b] = dens.label[in_sample[b]];
            membership[b] = dens.strength[in_sample[b]];
            continue;
        }
        tree.query(&z[b * d], kq, idx.data(), sq.data());
        int best = idx[0];
        double reach = std::numeric_limits<double>::infinity();
        for (int t = 0; t < kq; ++t) {
            const double r = std::max(core[idx[t]], std::sqrt(sq[t]));
            if (r < reach) reach = r, best = idx[t];
        }
        const int c = dens.label[best];
        if (c >= 0 && reach <= dens.birth[c]) {
            label[b] = c;
            membership[b] = dens.strength[best];
        }
    }

    // per-cluster diagonal Gaussians (original units): mean, width and the
    // fraction of members missing each dimension
    std::vector<double> w_(k0), mu_(static_cast<size_t>(k0) * d), sd_(static_cast<size_t>(k0) * d),
        miss_(static_cast<size_t>(k0) * d);
    auto describe = [&]() {
        std::vector<double> cnt(k0, 0.0), fin(k0 * d, 0.0), sum(k0 * d, 0.0), sum2(k0 * d, 0.0),
            fl2(k0 * d, 0.0), nfl(k0 * d, 0.0);
        for (size_t b : rows) {
            const int c = label[b];
            if (c < 0) continue;
            cnt[c] += 1;
            for (int j = 0; j < d; ++j) {
                const double v = x[b * d + j];
                if (std::isfinite(v)) fin[c * d + j] += 1, sum[c * d + j] += v, sum2[c * d + j] += v * v;
                const double f = width_floor.empty() ? kNaN : width_floor[b * d + j];
                if (std::isfinite(v) && std::isfinite(f)) fl2[c * d + j] += f * f, nfl[c * d + j] += 1;
            }
        }
        const double total = std::accumulate(cnt.begin(), cnt.end(), 0.0);
        for (int c = 0; c < k0; ++c) {
            w_[c] = cnt[c] / total;
            for (int j = 0; j < d; ++j) {
                const double f = fin[c * d + j];
                miss_[c * d + j] = std::min(std::max((cnt[c] - f) / std::max(cnt[c], 1.0), 1e-3), 1.0 - 1e-3);
                mu_[c * d + j] = sd_[c * d + j] = kNaN;
                if (f < 2) continue;
                const double mu = sum[c * d + j] / f;
                mu_[c * d + j] = mu;
                double sd = std::max(std::sqrt(std::max(sum2[c * d + j] / f - mu * mu, 0.0)), 1e-3 * wid[j]);
                // no species is narrower than its shot noise (a leaf that cut a
                // population in two is narrower than the population)
                if (nfl[c * d + j] > 0) sd = std::max(sd, std::sqrt(fl2[c * d + j] / nfl[c * d + j]));
                sd_[c * d + j] = sd;
            }
        }
    };
    // log density of row b in cluster c; `maha` is the squared distance over the
    // dimensions both have, `n_obs` their number
    auto loglik = [&](size_t b, int c, double& maha, int& n_obs) {
        double l = std::log(std::max(w_[c], 1e-300));
        maha = 0.0, n_obs = 0;
        for (int j = 0; j < d; ++j) {
            const double v = x[b * d + j], mu = mu_[c * d + j], s = sd_[c * d + j];
            if (!std::isfinite(v)) {
                l += std::log(miss_[c * d + j]);
            } else if (!std::isfinite(mu)) {  // a value this cluster never has
                l += std::log(1e-3) - std::log(std::max(hi[j] - lo[j], wid[j]));
                maha = std::numeric_limits<double>::infinity();
            } else {
                const double u = (v - mu) / s;
                l += std::log(1.0 - miss_[c * d + j]) - 0.5 * (kLog2Pi + u * u) - std::log(s);
                maha += u * u, ++n_obs;
            }
        }
        return l;
    };

    // the density core of a shot-noise-broadened population leaves its tails
    // unclaimed, and population means need them: an unclaimed burst joins its
    // most likely cluster when it lies inside that cluster's 99.9% ellipsoid
    // (three rounds, the widths growing with the tails); the rest stay noise
    static const double kChi2[] = {0.0, 10.83, 13.82, 16.27, 18.47, 20.52, 22.46, 24.32, 26.12};
    for (int round = 0; round < 3; ++round) {
        describe();
        bool grew = false;
        for (size_t b : rows) {
            if (label[b] >= 0) continue;
            int best = -1, n_best = 0;
            double top = -std::numeric_limits<double>::infinity(), maha_best = 0.0;
            for (int c = 0; c < k0; ++c) {
                double maha;
                int n_obs;
                const double l = loglik(b, c, maha, n_obs);
                if (l > top) top = l, best = c, maha_best = maha, n_best = n_obs;
            }
            const double cut = n_best < 9 ? kChi2[n_best] : 2.0 * n_best + 8.0;
            if (best >= 0 && n_best > 0 && maha_best <= cut) label[b] = best, grew = true;
        }
        if (!grew) break;
    }
    describe();

    // numbered by first-dimension mean
    std::vector<int> order(k0);
    std::iota(order.begin(), order.end(), 0);
    auto mean0 = [&](int c) { return mu_[c * d]; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const double ma = mean0(a), mb = mean0(b);
        return std::isfinite(ma) && (!std::isfinite(mb) || ma < mb);
    });
    std::vector<int> rank(k0);
    for (int r = 0; r < k0; ++r) rank[order[r]] = r;

    MixtureNdResult fit;
    fit.n_components = k0;
    fit.n_dims = d;
    fit.means.assign(static_cast<size_t>(k0) * d, kNaN);
    fit.sigmas.assign(static_cast<size_t>(k0) * d, kNaN);
    fit.weights.assign(k0, 0.0);
    for (int c = 0; c < k0; ++c) {
        fit.weights[rank[c]] = w_[c];
        for (int j = 0; j < d; ++j) {
            fit.means[rank[c] * d + j] = mu_[c * d + j];
            fit.sigmas[rank[c] * d + j] = sd_[c * d + j];
        }
    }
    fit.labels.assign(n, -1);
    fit.responsibilities.assign(n * k0, kNaN);
    std::vector<double> lp(k0);
    for (size_t b : rows) {
        if (label[b] < 0) continue;
        fit.labels[b] = rank[label[b]];
        double top = -std::numeric_limits<double>::infinity();
        for (int c = 0; c < k0; ++c) {
            double maha;
            int n_obs;
            lp[c] = loglik(b, c, maha, n_obs);
            top = std::max(top, lp[c]);
        }
        double norm = 0.0;
        for (int c = 0; c < k0; ++c) norm += std::exp(lp[c] - top);
        for (int c = 0; c < k0; ++c) fit.responsibilities[b * k0 + rank[c]] = std::exp(lp[c] - top) / norm;
    }

    // a species is at least as wide as its shot noise: the members' rms floor
    std::vector<double> floor_kd;
    if (!width_floor.empty()) {
        floor_kd.assign(static_cast<size_t>(k0) * d, 0.0);
        std::vector<double> cnt(static_cast<size_t>(k0) * d, 0.0);
        for (size_t b : rows) {
            if (label[b] < 0) continue;
            const size_t r = static_cast<size_t>(rank[label[b]]);
            for (int j = 0; j < d; ++j) {
                const double f = width_floor[b * d + j];
                if (std::isfinite(f)) floor_kd[r * d + j] += f * f, cnt[r * d + j] += 1;
            }
        }
        for (size_t i = 0; i < floor_kd.size(); ++i)
            floor_kd[i] = cnt[i] > 0 ? std::sqrt(floor_kd[i] / cnt[i]) : kNaN;
    }
    PopulationSplit out = afret_detail::split_from_components(fit, n_rows, names, donor_only_above,
                                                              acceptor_only_below, min_population,
                                                              min_probability, floor_kd);
    out.method = "hdbscan";
    out.noise.assign(n, 0);
    out.cluster_labels = fit.labels;
    out.membership = membership;
    for (size_t b : rows) out.noise[b] = label[b] < 0;
    return out;
}

} // namespace tttrlib
