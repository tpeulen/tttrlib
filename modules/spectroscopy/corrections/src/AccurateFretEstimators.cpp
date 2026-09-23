// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretCalibrate.h"
#include "AccurateFretDetail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {
const double kNaN = std::numeric_limits<double>::quiet_NaN();
}

std::vector<double> global_es_correction(const std::vector<double>& g,
                                         const std::vector<double>& r,
                                         const std::vector<double>& y,
                                         const std::vector<int>& labels, double alpha,
                                         double delta) {
    require_same_length(g, r, "global_es_correction");
    require_same_length(g, y, "global_es_correction");
    if (labels.size() != g.size())
        throw std::invalid_argument("global_es_correction: labels differ in length");
    const size_t n = g.size();
    std::vector<double> e_pr(n), s_pr(n);
    for (size_t b = 0; b < n; ++b) {
        const double f_da = (r[b] - alpha * g[b]) - delta * y[b];
        const double gf = g[b] + f_da;
        e_pr[b] = gf != 0.0 ? f_da / gf : 0.0;
        s_pr[b] = (gf + y[b]) != 0.0 ? gf / (gf + y[b]) : 0.0;
    }
    const std::set<int> uniq(labels.begin(), labels.end());
    if (uniq.size() < 2) throw std::invalid_argument("global_es_correction needs >= 2 populations");
    std::vector<double> xm, ym, w;
    for (int u : uniq) {
        std::vector<double> ev, sv;
        for (size_t b = 0; b < n; ++b)
            if (labels[b] == u) { ev.push_back(e_pr[b]); sv.push_back(s_pr[b]); }
        xm.push_back(np_mean(ev));
        ym.push_back(1.0 / np_mean(sv));
        w.push_back(std::sqrt(static_cast<double>(ev.size())));
    }
    // np.polyfit(x, y, 1, w): columns [x w, w] scaled to unit norm, least squares,
    // then unscaled; the 2x2 normal equations of the scaled system are well posed
    const size_t m = xm.size();
    std::vector<double> c0(m), c1(m), rhs(m);
    double n0 = 0.0, n1 = 0.0;
    for (size_t i = 0; i < m; ++i) {
        c0[i] = xm[i] * w[i];
        c1[i] = 1.0 * w[i];
        rhs[i] = ym[i] * w[i];
        n0 += c0[i] * c0[i];
        n1 += c1[i] * c1[i];
    }
    n0 = std::sqrt(n0);
    n1 = std::sqrt(n1);
    double a00 = 0, a01 = 0, a11 = 0, b0 = 0, b1 = 0;
    for (size_t i = 0; i < m; ++i) {
        const double u = c0[i] / n0, v = c1[i] / n1;
        a00 += u * u; a01 += u * v; a11 += v * v;
        b0 += u * rhs[i]; b1 += v * rhs[i];
    }
    const double det = a00 * a11 - a01 * a01;
    const double sigma = ((a11 * b0 - a01 * b1) / det) / n0;
    const double omega = ((a00 * b1 - a01 * b0) / det) / n1;
    const double denom = omega + sigma - 1.0;
    const double gamma = std::abs(denom) > 1e-12 ? (omega - 1.0) / denom : kNaN;
    return {gamma, denom, omega, sigma};
}

double beta_from_stoichiometry(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                               const std::vector<double>& i_aa, const FretFactors& f,
                               double target) {
    require_same_length(i_dd, i_da, "beta_from_stoichiometry");
    require_same_length(i_dd, i_aa, "beta_from_stoichiometry");
    const size_t n = i_dd.size();
    std::vector<double> num(n), den(n);
    for (size_t b = 0; b < n; ++b) {
        const double f_dd = i_dd[b] - f.bg_dd;
        const double f_aa = i_aa[b] - f.bg_aa;
        const double f_da = ((i_da[b] - f.bg_da) - f.alpha * f_dd) - f.delta * f_aa;
        num[b] = f.gamma * f_dd + f_da;
        den[b] = f_aa;
    }
    const double nm = np_mean(num), dm = np_mean(den);
    const double t = std::min(std::max(target, 1e-6), 1.0 - 1e-6);
    if (nm <= 0 || dm <= 0) return 1.0;
    return dm / (nm * (1.0 / t - 1.0));
}

double np_interp(double x, const std::vector<double>& xp, const std::vector<double>& fp) {
    if (!std::isfinite(x)) return kNaN;
    const size_t n = xp.size();
    if (n == 0) throw std::invalid_argument("FRET line is empty");
    if (x < xp[0]) return fp[0];
    if (x > xp[n - 1]) return fp[n - 1];
    if (x == xp[n - 1]) return fp[n - 1];
    const size_t j = static_cast<size_t>(std::upper_bound(xp.begin(), xp.end(), x) - xp.begin()) - 1;
    const double slope = (fp[j + 1] - fp[j]) / (xp[j + 1] - xp[j]);
    double v = slope * (x - xp[j]) + fp[j];
    if (std::isnan(v)) {
        v = slope * (x - xp[j + 1]) + fp[j + 1];
        if (std::isnan(v) && fp[j] == fp[j + 1]) v = fp[j];
    }
    return v;
}

LifetimeGamma gamma_from_lifetime(const std::vector<double>& i_dd,
                                  const std::vector<double>& i_da,
                                  const std::vector<double>& tau_f,
                                  const std::vector<double>& line_tau,
                                  const std::vector<double>& line_e,
                                  const std::vector<double>& i_aa, const FretFactors& f,
                                  const std::vector<int>& labels_in, int min_population,
                                  double e_min, double e_max) {
    require_same_length(i_dd, i_da, "gamma_from_lifetime");
    require_same_length(i_dd, tau_f, "gamma_from_lifetime");
    if (line_tau.size() != line_e.size() || line_tau.empty())
        throw std::invalid_argument("gamma_from_lifetime: the FRET line needs matching, non-empty tau_f and E");
    const size_t n = i_dd.size();
    const bool alex = !i_aa.empty();
    if (alex) require_same_length(i_dd, i_aa, "gamma_from_lifetime");
    std::vector<int> labels = labels_in.empty() ? std::vector<int>(n, 0) : labels_in;
    if (labels.size() != n) throw std::invalid_argument("gamma_from_lifetime: labels differ in length");
    std::vector<double> f_dd(n), f_da(n);
    for (size_t b = 0; b < n; ++b) {
        f_dd[b] = i_dd[b] - f.bg_dd;
        f_da[b] = (i_da[b] - f.bg_da) - f.alpha * f_dd[b];
        if (alex && f.delta != 0.0) f_da[b] = f_da[b] - f.delta * (i_aa[b] - f.bg_aa);
    }
    LifetimeGamma out;
    const std::set<int> uniq(labels.begin(), labels.end());
    for (int u : uniq) {
        std::vector<double> t, dd, da;
        for (size_t b = 0; b < n; ++b)
            if (labels[b] == u && std::isfinite(tau_f[b]) && std::isfinite(f_dd[b]) && std::isfinite(f_da[b])) {
                t.push_back(tau_f[b]); dd.push_back(f_dd[b]); da.push_back(f_da[b]);
            }
        if (static_cast<int>(t.size()) < min_population) continue;
        const double tau_mean = np_mean(t);
        const double e_line = np_interp(tau_mean, line_tau, line_e);
        const double mdd = np_mean(dd), mda = np_mean(da);
        if (!(e_min <= e_line && e_line <= e_max) || mdd <= 0 || mda <= 0) continue;
        out.labels.push_back(u);
        out.n.push_back(static_cast<int>(t.size()));
        out.tau_f.push_back(tau_mean);
        out.e_line.push_back(e_line);
        out.gammas.push_back((mda / mdd) * (1.0 - e_line) / e_line);
    }
    if (out.gammas.empty()) { out.gamma = kNaN; out.sigma = kNaN; return out; }
    const size_t k = out.gammas.size();
    std::vector<double> w(k), wv(k);
    for (size_t i = 0; i < k; ++i) { w[i] = out.n[i]; wv[i] = w[i] * out.gammas[i]; }
    out.gamma = np_sum(wv) / np_sum(w);
    if (k > 1) {
        std::vector<double> wd(k);
        for (size_t i = 0; i < k; ++i) {
            const double d = out.gammas[i] - out.gamma;
            wd[i] = w[i] * (d * d);
        }
        out.sigma = std::sqrt((np_sum(wd) / np_sum(w)) / static_cast<double>(k));
    } else {
        out.sigma = kNaN;
    }
    return out;
}

std::vector<double> lightpath_correction_factors(double c_gd, double c_rd, double c_ra,
                                                 double ex_ag, double ex_ar, double gG,
                                                 double gR, double qy_d, double qy_a) {
    const double eps = 1e-12;
    const double den_g = gG * c_gd * qy_d;
    const double gamma = den_g > eps ? (gR * c_ra * qy_a) / den_g : kNaN;
    const double den_a = gG * c_gd;
    const double alpha = den_a > eps ? (gR * c_rd) / den_a : 0.0;
    const double delta = std::abs(ex_ar) > eps ? ex_ag / ex_ar : 0.0;
    return {gamma, alpha, delta};
}

} // namespace tttrlib
