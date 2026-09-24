// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretPopulations.h"
#include "AccurateFretDetail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

std::vector<int> split_fret_subpopulations(const std::vector<double>& e, int max_populations,
                                           int min_population, double min_separation,
                                           double min_fraction) {
    std::vector<int> zeros(e.size(), 0);
    std::vector<size_t> fin;
    for (size_t i = 0; i < e.size(); ++i)
        if (std::isfinite(e[i])) fin.push_back(i);
    if (fin.size() < static_cast<size_t>(2 * min_population) || max_populations < 2) return zeros;
    MixtureResult fit = best_gaussian_mixture_1d(e, max_populations);
    std::vector<int> lab(e.size(), 0);
    for (size_t f = 0; f < fin.size(); ++f) lab[fin[f]] = fit.labels[f];

    const std::vector<double>& means = fit.means;
    const long floor_n = std::max<long>(min_population,
        static_cast<long>(std::ceil(min_fraction * static_cast<double>(fin.size()))));
    std::vector<int> keep;
    for (int i = 0; i < static_cast<int>(means.size()); ++i) {
        long n_i = 0;
        for (size_t f : fin) n_i += lab[f] == i;
        if (n_i < floor_n) continue;
        if (!keep.empty() && std::abs(means[i] - means[keep.back()]) < min_separation) continue;
        keep.push_back(i);
    }
    if (keep.size() < 2) return zeros;
    std::vector<int> target(means.size());
    for (int i = 0; i < static_cast<int>(means.size()); ++i) {
        auto it = std::find(keep.begin(), keep.end(), i);
        if (it != keep.end()) { target[i] = static_cast<int>(it - keep.begin()); continue; }
        // attach to the nearest kept component
        size_t best = 0;
        for (size_t c = 1; c < keep.size(); ++c)
            if (std::abs(means[i] - means[keep[c]]) < std::abs(means[i] - means[keep[best]])) best = c;
        target[i] = static_cast<int>(best);
    }
    std::vector<int> out(e.size(), 0);
    for (size_t b = 0; b < e.size(); ++b) out[b] = target[lab[b]];
    return out;
}

PopulationSplit classify_es_populations(const std::vector<double>& s,
                                        const std::vector<double>& efficiency,
                                        double donor_only_above, double acceptor_only_below,
                                        int max_components, int max_fret_populations,
                                        int min_population, double reference_sigma,
                                        const std::string& method) {
    if (!efficiency.empty() && efficiency.size() != s.size())
        throw std::invalid_argument("classify_es_populations: S and E differ in length");
    const size_t n = s.size();
    PopulationSplit out;
    double lo = acceptor_only_below, hi = donor_only_above;
    bool have_donor_core = false, have_acceptor_core = false;
    double donor_core = 0.0, acceptor_core = 0.0;
    const std::vector<double> sf = finite_only(s);
    if (method != "threshold" && sf.size() >= 15) {
        try {
            MixtureResult fit = best_gaussian_mixture_1d(sf, max_components);
            const auto& m = fit.means;
            const auto& sg = fit.sigmas;
            std::vector<int> klass(m.size());
            for (size_t c = 0; c < m.size(); ++c)
                klass[c] = m[c] >= hi ? 1 : (m[c] <= lo ? -1 : 0);
            if (std::count(klass.begin(), klass.end(), 0) > 0) {
                double fret_min = std::numeric_limits<double>::infinity();
                double fret_max = -std::numeric_limits<double>::infinity();
                for (size_t c = 0; c < m.size(); ++c)
                    if (klass[c] == 0) { fret_min = std::min(fret_min, m[c]); fret_max = std::max(fret_max, m[c]); }
                int acc = -1, don = -1;
                for (size_t c = 0; c < m.size(); ++c) {
                    if (klass[c] == -1 && (acc < 0 || m[c] > m[acc])) acc = static_cast<int>(c);
                    if (klass[c] == 1 && (don < 0 || m[c] < m[don])) don = static_cast<int>(c);
                }
                if (acc >= 0) {
                    lo = 0.5 * (m[acc] + fret_min);
                    if (reference_sigma > 0) { acceptor_core = m[acc] + reference_sigma * sg[acc]; have_acceptor_core = true; }
                }
                if (don >= 0) {
                    hi = 0.5 * (fret_max + m[don]);
                    if (reference_sigma > 0) { donor_core = m[don] - reference_sigma * sg[don]; have_donor_core = true; }
                }
                out.method = "mixture";
                out.component_means = m;
                out.component_weights = fit.weights;
                out.component_sigmas = sg;
                out.bic_k = fit.bic_k;
                out.bic_values = fit.bic_values;
            }
        } catch (const std::exception&) {
            out.method = "threshold";
        }
    }
    const double donor_cut = std::max(hi, have_donor_core ? donor_core : hi);
    const double acceptor_cut = std::min(lo, have_acceptor_core ? acceptor_core : lo);
    out.donor_only.assign(n, 0);
    out.acceptor_only.assign(n, 0);
    out.fret.assign(n, 0);
    long nd = 0, na = 0, nf = 0;
    for (size_t b = 0; b < n; ++b) {
        if (!std::isfinite(s[b])) continue;
        out.donor_only[b] = s[b] > donor_cut;
        out.acceptor_only[b] = s[b] < acceptor_cut;
        out.fret[b] = !(s[b] > hi) && !(s[b] < lo);
        nd += out.donor_only[b];
        na += out.acceptor_only[b];
        nf += out.fret[b];
    }
    if (nd < min_population) std::fill(out.donor_only.begin(), out.donor_only.end(), 0);
    if (na < min_population) std::fill(out.acceptor_only.begin(), out.acceptor_only.end(), 0);

    out.fret_labels.assign(n, -1);
    if (!efficiency.empty() && nf >= min_population) {
        std::vector<double> ef;
        for (size_t b = 0; b < n; ++b)
            if (out.fret[b]) ef.push_back(efficiency[b]);
        std::vector<int> sub = split_fret_subpopulations(ef, max_fret_populations, min_population);
        size_t f = 0;
        for (size_t b = 0; b < n; ++b)
            if (out.fret[b]) out.fret_labels[b] = sub[f++];
    } else if (nf > 0) {
        for (size_t b = 0; b < n; ++b)
            if (out.fret[b]) out.fret_labels[b] = 0;
    }
    out.threshold_lo = lo;
    out.threshold_hi = hi;
    return out;
}

double leakage_from_donor_only(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                               double bg_dd, double bg_da) {
    require_same_length(i_dd, i_da, "leakage_from_donor_only");
    std::vector<double> f_dd(i_dd.size()), f_da(i_da.size());
    for (size_t b = 0; b < i_dd.size(); ++b) { f_dd[b] = i_dd[b] - bg_dd; f_da[b] = i_da[b] - bg_da; }
    const double denom = np_mean(f_dd);
    return denom != 0.0 ? np_mean(f_da) / denom : 0.0;
}

double direct_excitation_from_acceptor_only(const std::vector<double>& i_da,
                                            const std::vector<double>& i_aa,
                                            const std::vector<double>& i_dd, double alpha,
                                            double bg_dd, double bg_da, double bg_aa) {
    require_same_length(i_da, i_aa, "direct_excitation_from_acceptor_only");
    const bool leak = !i_dd.empty() && alpha != 0.0;
    if (leak) require_same_length(i_da, i_dd, "direct_excitation_from_acceptor_only");
    std::vector<double> f_da(i_da.size()), f_aa(i_aa.size());
    for (size_t b = 0; b < i_da.size(); ++b) {
        f_da[b] = i_da[b] - bg_da;
        if (leak) f_da[b] = f_da[b] - alpha * (i_dd[b] - bg_dd);
        f_aa[b] = i_aa[b] - bg_aa;
    }
    const double denom = np_mean(f_aa);
    return denom != 0.0 ? np_mean(f_da) / denom : 0.0;
}

} // namespace tttrlib
