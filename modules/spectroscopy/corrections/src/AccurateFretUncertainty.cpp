// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretUncertainty.h"
#include "AccurateFretDetail.h"

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {
const double kNaN = std::numeric_limits<double>::quiet_NaN();
double finite_or_zero(double v) { return std::isfinite(v) ? v : 0.0; }
}

EfficiencyUncertainty efficiency_uncertainty(const std::vector<double>& e,
                                             const std::vector<double>& dd,
                                             const std::vector<double>& aa, double gamma,
                                             double sg, double sa, double sd,
                                             const std::vector<double>& stat) {
    require_same_length(e, dd, "efficiency_uncertainty");
    if (!aa.empty()) require_same_length(e, aa, "efficiency_uncertainty");
    if (!stat.empty()) require_same_length(e, stat, "efficiency_uncertainty");
    const double g = gamma != 0.0 ? gamma : 1.0;
    const size_t n = e.size();
    EfficiencyUncertainty u;
    u.total.resize(n); u.systematic.resize(n); u.statistical.resize(n);
    u.d_gamma.resize(n); u.d_alpha.resize(n); u.d_delta.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const double om = 1.0 - e[i];
        u.d_gamma[i] = std::abs(e[i] * om / g) * sg;
        u.d_alpha[i] = std::abs(om * om / g) * sa;
        double ratio = 0.0;
        if (!aa.empty()) ratio = dd[i] != 0.0 ? aa[i] / (g * dd[i]) : 0.0;
        u.d_delta[i] = aa.empty() ? 0.0 : std::abs(om * om * ratio) * sd;
        u.systematic[i] = std::sqrt(u.d_gamma[i] * u.d_gamma[i] + u.d_alpha[i] * u.d_alpha[i]
                                    + u.d_delta[i] * u.d_delta[i]);
        u.statistical[i] = stat.empty() ? 0.0 : std::abs(stat[i]);
        u.total[i] = std::sqrt(u.systematic[i] * u.systematic[i] + u.statistical[i] * u.statistical[i]);
    }
    return u;
}

DistanceResult distance_from_efficiency(const std::vector<double>& e, double r0,
                                        const std::vector<double>& sigma_e, double sigma_r0) {
    if (!sigma_e.empty()) require_same_length(e, sigma_e, "distance_from_efficiency");
    DistanceResult d;
    d.distance.resize(e.size());
    d.sigma.resize(e.size());
    const double rel_r0 = r0 != 0.0 ? sigma_r0 / r0 : 0.0;
    for (size_t i = 0; i < e.size(); ++i) {
        const bool valid = e[i] > 0.0 && e[i] < 1.0;
        const double r = valid ? r0 * std::pow(1.0 / e[i] - 1.0, 1.0 / 6.0) : kNaN;
        double rel_e = 0.0;
        if (!sigma_e.empty()) rel_e = valid ? std::abs(sigma_e[i]) / (6.0 * e[i] * (1.0 - e[i])) : kNaN;
        d.distance[i] = r;
        d.sigma[i] = r * std::sqrt(rel_r0 * rel_r0 + rel_e * rel_e);
    }
    return d;
}

AccurateFretResult accurate_fret(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                                 const std::vector<double>& i_aa, const FretFactors& f,
                                 double sigma_gamma, double sigma_alpha, double sigma_delta,
                                 double sigma_r0, const std::vector<double>& tau_f,
                                 const std::vector<double>& line_tau,
                                 const std::vector<double>& line_e,
                                 const std::vector<int>& labels_in) {
    const double sg = finite_or_zero(sigma_gamma), sa = finite_or_zero(sigma_alpha);
    const double sd = finite_or_zero(sigma_delta), sr = finite_or_zero(sigma_r0);
    const size_t n = i_dd.size();
    if (!tau_f.empty()) require_same_length(i_dd, tau_f, "accurate_fret");
    if (line_tau.size() != line_e.size())
        throw std::invalid_argument("accurate_fret: FRET line tau_f and efficiency differ in length");
    std::vector<int> labels = labels_in.empty() ? std::vector<int>(n, 0) : labels_in;
    if (labels.size() != n) throw std::invalid_argument("accurate_fret: labels differ in length");

    AccurateFretResult r;
    r.factors = f;
    r.es = corrected_es(i_dd, i_da, i_aa, f);
    std::vector<double> f_dd(n), f_aa;
    for (size_t b = 0; b < n; ++b) f_dd[b] = i_dd[b] - f.bg_dd;
    for (double v : i_aa) f_aa.push_back(v - f.bg_aa);
    const EfficiencyUncertainty u = efficiency_uncertainty(r.es.E, f_dd, f_aa, f.gamma, sg, sa, sd);
    const DistanceResult dist = distance_from_efficiency(r.es.E, f.r0, u.total, sr);
    r.sigma_E = u.total;
    r.sigma_E_systematic = u.systematic;
    r.distance = dist.distance;
    r.sigma_distance = dist.sigma;
    const bool line = !tau_f.empty() && !line_tau.empty();
    if (line) {
        r.has_deviation = true;
        for (size_t b = 0; b < n; ++b) r.deviation.push_back(r.es.E[b] - np_interp(tau_f[b], line_tau, line_e));
    }

    const std::set<int> uniq(labels.begin(), labels.end());
    for (int u_label : uniq) {
        std::vector<double> em, sm, sys, tm;
        int n_fin = 0;
        for (size_t b = 0; b < n; ++b) {
            if (labels[b] != u_label) continue;
            em.push_back(r.es.E[b]);
            if (r.es.has_s && !r.es.S.empty()) sm.push_back(r.es.S[b]);
            sys.push_back(u.systematic[b]);
            if (!tau_f.empty() && std::isfinite(tau_f[b])) tm.push_back(tau_f[b]);
            n_fin += std::isfinite(r.es.E[b]);
        }
        if (n_fin == 0) continue;
        FretPopulation p;
        p.label = u_label;
        p.n = n_fin;
        p.E = np_nanmean(em);
        p.sigma_E_statistical = np_nanstd(em) / std::max(std::sqrt(static_cast<double>(n_fin)), 1.0);
        p.sigma_E_systematic = np_nanmean(sys);
        p.sigma_E = std::hypot(p.sigma_E_systematic, p.sigma_E_statistical);
        const DistanceResult pd = distance_from_efficiency({p.E}, f.r0, {p.sigma_E}, sr);
        p.distance = pd.distance[0];
        p.sigma_distance = pd.sigma[0];
        p.S = sm.empty() ? kNaN : np_nanmean(sm);
        if (!tau_f.empty()) {
            p.has_tau = true;
            p.tau_f = tm.empty() ? kNaN : np_mean(tm);
            if (!line_tau.empty() && std::isfinite(p.tau_f)) {
                p.has_line = true;
                p.E_line = np_interp(p.tau_f, line_tau, line_e);
                p.deviation = p.E - p.E_line;
            }
        }
        r.populations.push_back(p);
    }
    return r;
}

} // namespace tttrlib
