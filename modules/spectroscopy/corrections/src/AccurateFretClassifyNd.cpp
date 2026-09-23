// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretMultiDim.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tttrlib {

namespace {

int index_of(const std::vector<std::string>& names, const char* name) {
    auto it = std::find(names.begin(), names.end(), name);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}

} // namespace

PopulationSplit classify_populations_nd(const std::vector<double>& x, int n_rows,
                                        const std::vector<std::string>& names,
                                        double donor_only_above, double acceptor_only_below,
                                        int max_components, int min_population,
                                        double min_probability) {
    const int d = static_cast<int>(names.size());
    if (d == 0 || x.size() != static_cast<size_t>(n_rows) * d)
        throw std::invalid_argument("classify_populations_nd: x must be (n_rows, len(names))");
    const MixtureNdResult fit = best_gaussian_mixture_nd(x, n_rows, d, max_components);
    const int k = fit.n_components;
    const int iS = index_of(names, "S"), iE = index_of(names, "E"), iT = index_of(names, "tau_d");
    auto centre = [&](int c, int j) { return fit.means[static_cast<size_t>(c) * d + j]; };

    // class per component: 1 donor-only, -1 acceptor-only, 0 FRET
    std::vector<int> klass(k, 0);
    double longest = -std::numeric_limits<double>::infinity();
    if (iT >= 0)
        for (int c = 0; c < k; ++c) longest = std::max(longest, centre(c, iT));
    for (int c = 0; c < k; ++c) {
        if (iS >= 0) {
            klass[c] = centre(c, iS) >= donor_only_above ? 1 : (centre(c, iS) <= acceptor_only_below ? -1 : 0);
        } else if (iE >= 0 && centre(c, iE) < 0.1 && (iT < 0 || centre(c, iT) >= 0.9 * longest)) {
            klass[c] = 1;
        }
    }

    // FRET components: largest first; one lying within a width of a kept one in
    // every dimension is the same species split by a non-Gaussian shape (shot
    // noise skews E) and is merged, as is one below min_population
    const int iOrder = iE >= 0 ? iE : 0;
    auto sigma = [&](int c, int j) { return fit.sigmas[static_cast<size_t>(c) * d + j]; };
    auto same = [&](int a, int b) {
        for (int j = 0; j < d; ++j)
            if (std::abs(centre(a, j) - centre(b, j)) >= std::max(sigma(a, j), sigma(b, j))) return false;
        return true;
    };
    auto distance = [&](int a, int b) {
        double q = 0.0;
        for (int j = 0; j < d; ++j) {
            const double s2 = sigma(a, j) * sigma(a, j) + sigma(b, j) * sigma(b, j);
            q += (centre(a, j) - centre(b, j)) * (centre(a, j) - centre(b, j)) / s2;
        }
        return q;
    };
    std::vector<long> hard(k, 0);
    for (int i = 0; i < n_rows; ++i)
        if (fit.labels[i] >= 0) hard[fit.labels[i]] += 1;
    std::vector<int> fret_comp;
    for (int c = 0; c < k; ++c)
        if (klass[c] == 0) fret_comp.push_back(c);
    std::stable_sort(fret_comp.begin(), fret_comp.end(), [&](int a, int b) { return hard[a] > hard[b]; });
    std::vector<int> kept;
    for (int c : fret_comp) {
        bool merged = false;
        for (int t : kept) merged = merged || same(c, t);
        if (!merged && (hard[c] >= min_population || kept.empty())) kept.push_back(c);
    }
    std::stable_sort(kept.begin(), kept.end(), [&](int a, int b) { return centre(a, iOrder) < centre(b, iOrder); });
    std::vector<int> target(k, -1);
    for (int c : fret_comp) {
        size_t best = 0;
        for (size_t t = 0; t < kept.size(); ++t)
            if (kept[t] == c || distance(c, kept[t]) < distance(c, kept[best])) {
                best = t;
                if (kept[t] == c) break;
            }
        target[c] = static_cast<int>(best);
    }

    PopulationSplit out;
    out.method = "mixture_nd";
    out.dimensions = names;
    out.component_means = fit.means;
    out.component_weights = fit.weights;
    out.component_sigmas = fit.sigmas;
    out.bic_k = fit.bic_k;
    out.bic_values = fit.bic_values;
    out.threshold_lo = acceptor_only_below;
    out.threshold_hi = donor_only_above;
    const int nf = static_cast<int>(kept.size());
    out.n_fret_populations = nf;
    out.donor_only.assign(n_rows, 0);
    out.acceptor_only.assign(n_rows, 0);
    out.fret.assign(n_rows, 0);
    out.fret_labels.assign(n_rows, -1);
    out.fret_probabilities.assign(static_cast<size_t>(n_rows) * nf, 0.0);
    long nd = 0, na = 0;
    for (int i = 0; i < n_rows; ++i) {
        const int c = fit.labels[i];
        if (c < 0) continue;
        const double p = fit.responsibilities[static_cast<size_t>(i) * k + c];
        if (klass[c] == 1 && p >= min_probability) { out.donor_only[i] = 1; ++nd; }
        if (klass[c] == -1 && p >= min_probability) { out.acceptor_only[i] = 1; ++na; }
        if (klass[c] != 0) continue;
        out.fret[i] = 1;
        out.fret_labels[i] = target[c];
        double norm = 0.0;
        for (int f : fret_comp) {
            const double r = fit.responsibilities[static_cast<size_t>(i) * k + f];
            out.fret_probabilities[static_cast<size_t>(i) * nf + target[f]] += r;
            norm += r;
        }
        for (int t = 0; t < nf && norm > 0; ++t) out.fret_probabilities[static_cast<size_t>(i) * nf + t] /= norm;
    }
    if (nd < min_population) std::fill(out.donor_only.begin(), out.donor_only.end(), 0);
    if (na < min_population) std::fill(out.acceptor_only.begin(), out.acceptor_only.end(), 0);
    return out;
}

} // namespace tttrlib
