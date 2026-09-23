// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretCalibrate.h"
#include "AccurateFretUncertainty.h"
#include "AccurateFretDetail.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

// the bounded parameter group this replaces clamped on every write: max(lb, v), min(ub, v)
double clamp(double v, double lo, double hi) {
    v = (v > lo) ? v : lo;
    return (v < hi) ? v : hi;
}

void set_gamma(AutoCalibration& s, const AutoCalibrateOptions& o, double v) { s.factors.gamma = clamp(v, o.gamma_lo, o.gamma_hi); }
void set_alpha(AutoCalibration& s, const AutoCalibrateOptions& o, double v) { s.factors.alpha = clamp(v, o.alpha_lo, o.alpha_hi); }
void set_beta(AutoCalibration& s, const AutoCalibrateOptions& o, double v) { s.factors.beta = clamp(v, o.beta_lo, o.beta_hi); }
void set_delta(AutoCalibration& s, const AutoCalibrateOptions& o, double v) { s.factors.delta = clamp(v, o.delta_lo, o.delta_hi); }

std::vector<double> pick(const std::vector<double>& v, const std::vector<int>& mask) {
    std::vector<double> out;
    for (size_t b = 0; b < v.size(); ++b)
        if (mask[b]) out.push_back(v[b]);
    return out;
}

std::string fmt4(const char* pattern, double a, double b = 0, double c = 0, double d = 0, double e = 0) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c, d, e);
    return buf;
}

bool any(const std::vector<int>& m) { return std::find(m.begin(), m.end(), 1) != m.end(); }

double select_gamma(const std::string& source, double es, double lt, std::vector<std::string>& msg) {
    if (source == "es") return es;
    if (source == "lifetime") return lt;
    if (source == "combined") {
        std::vector<double> v;
        if (std::isfinite(es)) v.push_back(es);
        if (std::isfinite(lt)) v.push_back(lt);
        return v.empty() ? kNaN : np_mean(v);
    }
    if (std::isfinite(es)) return es;
    if (std::isfinite(lt)) {
        msg.push_back("fewer than two FRET populations: gamma taken from the donor "
                      "lifetime and the static FRET line");
        return lt;
    }
    msg.push_back("gamma could not be determined from the data; prior value kept");
    return kNaN;
}

} // namespace

AutoCalibration auto_calibrate_start(const std::vector<double>& i_dd,
                                     const std::vector<double>& i_da,
                                     const std::vector<double>& i_aa,
                                     const std::vector<double>& tau_f, const FretFactors& f,
                                     const AutoCalibrateOptions& o) {
    require_same_length(i_dd, i_da, "auto_calibrate");
    if (!i_aa.empty()) require_same_length(i_dd, i_aa, "auto_calibrate");
    if (!tau_f.empty()) require_same_length(i_dd, tau_f, "auto_calibrate");
    if (o.line_tau_f.size() != o.line_efficiency.size())
        throw std::invalid_argument("auto_calibrate: FRET line tau_f and efficiency differ in length");
    AutoCalibration s;
    s.data_dd = i_dd;
    s.data_da = i_da;
    s.data_aa = i_aa;
    s.data_tau = tau_f;
    s.factors = f;
    set_gamma(s, o, f.gamma);
    set_alpha(s, o, f.alpha);
    set_beta(s, o, f.beta);
    set_delta(s, o, f.delta);
    s.factors.bg_dd = clamp(f.bg_dd, o.bg_lo, o.bg_hi);
    s.factors.bg_da = clamp(f.bg_da, o.bg_lo, o.bg_hi);
    s.factors.bg_aa = clamp(f.bg_aa, o.bg_lo, o.bg_hi);
    s.factors.r0 = clamp(f.r0, o.r0_lo, o.r0_hi);
    s.sigma_alpha = s.sigma_delta = s.sigma_gamma = s.sigma_beta = s.sigma_r0 = kNaN;
    s.gamma_es = s.gamma_lifetime = s.gamma_prior = s.gamma_posterior = kNaN;
    s.gamma_lifetime_sigma = s.gamma_data = kNaN;
    s.previous = {s.factors.alpha, s.factors.delta, s.factors.gamma, s.factors.beta};
    return s;
}

bool auto_calibrate_iterate(AutoCalibration& s, const AutoCalibrateOptions& o) {
    s.iterations += 1;
    s.iteration_messages.clear();
    auto& msg = s.iteration_messages;
    const auto& dd = s.data_dd;
    const auto& da = s.data_da;
    const auto& aa = s.data_aa;
    const bool alex = !aa.empty();
    EsResult es = corrected_es(dd, da, aa, s.factors);
    if (!alex) {
        if (s.iterations == 1)
            s.messages.push_back("no acceptor-excitation channel: every burst is taken to be "
                                 "doubly labelled \xE2\x80\x94 singly labelled bursts left in the data bias "
                                 "gamma, so gate them out first");
        PopulationSplit p;
        p.donor_only.assign(dd.size(), 0);
        p.acceptor_only.assign(dd.size(), 0);
        p.fret.assign(dd.size(), 1);
        p.fret_labels = split_fret_subpopulations(es.E, o.max_fret_populations, o.min_population);
        p.threshold_lo = 0.0;
        p.threshold_hi = 1.0;
        p.method = "none";
        s.split = p;
    } else {
        s.split = classify_es_populations(es.S, es.E, o.donor_only_above, o.acceptor_only_below, 4,
                                          o.max_fret_populations, o.min_population);
    }
    s.has_split = true;
    const PopulationSplit& sp = s.split;

    // alpha, then delta with the new alpha
    s.estimated_alpha = s.estimated_delta = false;
    if (any(sp.donor_only)) {
        set_alpha(s, o, leakage_from_donor_only(pick(dd, sp.donor_only), pick(da, sp.donor_only),
                                                s.factors.bg_dd, s.factors.bg_da));
        s.estimated_alpha = true;
    } else {
        msg.push_back("no donor-only population in the data");
    }
    if (alex && any(sp.acceptor_only)) {
        const auto& m = sp.acceptor_only;
        set_delta(s, o, direct_excitation_from_acceptor_only(pick(da, m), pick(aa, m), pick(dd, m),
                                                             s.factors.alpha, s.factors.bg_dd,
                                                             s.factors.bg_da, s.factors.bg_aa));
        s.estimated_delta = true;
    } else {
        msg.push_back("no acceptor-only population in the data");
    }

    // gamma/beta from the 1/S vs E line
    double gamma_es = kNaN, beta_es = kNaN;
    std::vector<int> fret_only(dd.size(), 0), fl;
    for (size_t b = 0; b < dd.size(); ++b) fret_only[b] = sp.fret[b] && sp.fret_labels[b] >= 0;
    for (size_t b = 0; b < dd.size(); ++b)
        if (fret_only[b]) fl.push_back(sp.fret_labels[b]);
    if (alex && fl.size() >= 2 && std::set<int>(fl.begin(), fl.end()).size() >= 2) {
        std::vector<double> est = global_es_correction(pick(dd, fret_only), pick(da, fret_only),
                                                       pick(aa, fret_only), fl, s.factors.alpha,
                                                       s.factors.delta);
        if (std::isfinite(est[0]) && est[0] > 0) gamma_es = est[0];
        if (std::isfinite(est[1]) && est[1] > 0) beta_es = est[1];
    }
    s.gamma_es = gamma_es;

    if (!s.data_tau.empty() && !o.line_tau_f.empty()) {
        std::vector<int> labels(dd.size());
        for (size_t b = 0; b < dd.size(); ++b) labels[b] = sp.fret[b] ? sp.fret_labels[b] : -1;
        LifetimeGamma lt = gamma_from_lifetime(dd, da, s.data_tau, o.line_tau_f, o.line_efficiency,
                                               aa, s.factors, labels, o.min_population);
        s.gamma_lifetime = lt.gamma;
        s.gamma_lifetime_sigma = lt.sigma;
        s.has_lifetime_gamma = true;
    }

    const double gamma = select_gamma(o.gamma_source, s.gamma_es, s.gamma_lifetime, msg);
    s.estimated_gamma = std::isfinite(gamma) && gamma > 0;
    if (s.estimated_gamma) set_gamma(s, o, std::min(std::max(gamma, 0.01), 100.0));
    if (std::isfinite(beta_es) && beta_es > 0) {
        set_beta(s, o, beta_es);
    } else if (alex && o.assume_one_to_one && any(sp.fret)) {
        set_beta(s, o, beta_from_stoichiometry(pick(dd, sp.fret), pick(da, sp.fret),
                                               pick(aa, sp.fret), s.factors));
        msg.push_back("only one FRET population: beta defined by centring it at S = 0.5 "
                      "(1:1 labelling assumed)");
    }

    const std::vector<double> current = {s.factors.alpha, s.factors.delta, s.factors.gamma, s.factors.beta};
    // numpy's max propagates NaN, and NaN < tol is false: a NaN factor never converges
    bool small = true;
    for (size_t i = 0; i < 4; ++i)
        if (!(std::abs(current[i] - s.previous[i]) < o.tolerance)) small = false;
    s.previous = current;
    return small;
}

void auto_calibrate_cancel(AutoCalibration& s) {
    s.cancelled = true;
    s.messages.push_back("stopped by the caller after " + std::to_string(s.iterations) + " iteration(s)");
}

void auto_calibrate_finish(AutoCalibration& s, const AutoCalibrateOptions& o) {
    s.messages.insert(s.messages.end(), s.iteration_messages.begin(), s.iteration_messages.end());
    s.gamma_data = s.factors.gamma;
    // bootstrap spread, where there were more than two resampled values
    const std::vector<double>* boots[4] = {&s.boot_alpha, &s.boot_delta, &s.boot_gamma, &s.boot_beta};
    double* sigmas[4] = {&s.sigma_alpha, &s.sigma_delta, &s.sigma_gamma, &s.sigma_beta};
    for (int k = 0; k < 4; ++k)
        if (boots[k]->size() > 2) *sigmas[k] = np_std(*boots[k]);
    if (!std::isfinite(s.sigma_gamma))
        s.sigma_gamma = s.has_lifetime_gamma ? s.gamma_lifetime_sigma : kNaN;
    if (o.use_priors) {
        struct Item { const char* name; double mu, sp; bool est; double* sigma; int which; };
        Item items[3] = {
            {"gamma", o.gamma_prior_mu, o.gamma_prior_sigma, s.estimated_gamma, &s.sigma_gamma, 0},
            {"alpha", o.alpha_prior_mu, o.alpha_prior_sigma, s.estimated_alpha, &s.sigma_alpha, 1},
            {"delta", o.delta_prior_mu, o.delta_prior_sigma, s.estimated_delta, &s.sigma_delta, 2},
        };
        for (Item& it : items) {
            double* value = it.which == 0 ? &s.factors.gamma : it.which == 1 ? &s.factors.alpha : &s.factors.delta;
            auto set = [&](double v) {
                if (it.which == 0) set_gamma(s, o, v); else if (it.which == 1) set_alpha(s, o, v); else set_delta(s, o, v);
            };
            const bool has_prior = std::isfinite(it.sp) && it.sp >= 0 && std::isfinite(it.mu);
            double sigma_d = *it.sigma;
            if (!has_prior) {
                if (!it.est)
                    s.messages.push_back(std::string(it.name) + " was neither identified by the data nor "
                                         "constrained by the light path; it keeps its current value");
                continue;
            }
            if (it.which == 0) s.gamma_prior = it.mu;
            if (!it.est) {
                set(it.mu);
                *it.sigma = it.sp;
                s.messages.push_back(std::string(it.name) + fmt4(" not identifiable from the data; the "
                                     "light-path value %.4f \xC2\xB1 %.4f is used", it.mu, it.sp));
                continue;
            }
            const double v = *value;
            if (!std::isfinite(sigma_d) || sigma_d <= 0) sigma_d = std::max(std::abs(v) * 0.1, 1e-6);
            const double w_d = 1.0 / (sigma_d * sigma_d);
            const double sp = std::max(it.sp, 1e-9);
            const double w_p = 1.0 / (sp * sp);
            set((v * w_d + it.mu * w_p) / (w_d + w_p));
            *it.sigma = std::sqrt(1.0 / (w_d + w_p));
            s.messages.push_back(std::string(it.name) + fmt4(": data %.4f \xC2\xB1 %.4f combined with the "
                                 "light-path prior %.4f \xC2\xB1 %.4f \xE2\x86\x92 %.4f", v, sigma_d, it.mu,
                                 it.sp, *value));
        }
    }
    s.gamma_posterior = s.factors.gamma;

    // the final summary per FRET sub-population, with the adopted uncertainties
    std::vector<int> labels(s.data_dd.size(), -1);
    if (s.has_split)
        for (size_t b = 0; b < labels.size(); ++b) labels[b] = s.split.fret[b] ? s.split.fret_labels[b] : -1;
    AccurateFretResult fin = accurate_fret(s.data_dd, s.data_da, s.data_aa, s.factors, s.sigma_gamma,
                                           s.sigma_alpha, s.sigma_delta, s.sigma_r0, s.data_tau,
                                           o.line_tau_f, o.line_efficiency, labels);
    s.populations.clear();
    for (const FretPopulation& p : fin.populations)
        if (p.label != -1) s.populations.push_back(p);
}

AutoCalibration auto_calibrate(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                               const std::vector<double>& i_aa, const std::vector<double>& tau_f,
                               const FretFactors& factors, const AutoCalibrateOptions& o) {
    AutoCalibration s = auto_calibrate_start(i_dd, i_da, i_aa, tau_f, factors, o);
    for (int it = 1; it <= o.n_iterations; ++it)
        if (auto_calibrate_iterate(s, o)) { s.converged = true; break; }
    for (int k = 0; k < o.n_bootstrap; ++k) auto_calibrate_bootstrap(s, o);
    auto_calibrate_finish(s, o);
    return s;
}

} // namespace tttrlib
