// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretSpecies.h"
#include "AccurateFretDetail.h"
#include "Mat.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Pop {
    double n = 0, D = 0, A = 0, Y = 0, sS = 0, sE = 0, tau = kNaN, tau_a = kNaN, e_line = kNaN;
    bool lifetime = false;
};

double e_of(double A, double D, double g) { const double den = A + g * D; return den > 0 ? A / den : 0.0; }
double s_of(double A, double D, double Y, double g, double b) {
    const double num = g * D + A, den = num + Y / b;
    return den != 0 ? num / den : 0.0;
}

using Residuals = std::function<std::vector<double>(const std::vector<double>&)>;

// Levenberg-Marquardt on log-parameters; returns chi2, fills theta and its covariance diagonal
double fit(const Residuals& res, std::vector<double>& theta, std::vector<double>& var_theta) {
    const size_t k = theta.size();
    auto chi2 = [&](const std::vector<double>& t) {
        double c = 0.0;
        for (double r : res(t)) c += r * r;
        return c;
    };
    auto jacobian = [&](const std::vector<double>& t, std::vector<double>& r0) {
        r0 = res(t);
        std::vector<double> J(r0.size() * k);
        for (size_t j = 0; j < k; ++j) {
            std::vector<double> tp = t, tm = t;
            tp[j] += 1e-6; tm[j] -= 1e-6;
            const auto rp = res(tp), rm = res(tm);
            for (size_t i = 0; i < r0.size(); ++i) J[i * k + j] = (rp[i] - rm[i]) / 2e-6;
        }
        return J;
    };
    double lambda = 1e-3, c = chi2(theta);
    std::vector<double> r0, J, JtJ(k * k), Jtr(k);
    for (int it = 0; it < 200; ++it) {
        J = jacobian(theta, r0);
        std::fill(JtJ.begin(), JtJ.end(), 0.0);
        std::fill(Jtr.begin(), Jtr.end(), 0.0);
        for (size_t i = 0; i < r0.size(); ++i)
            for (size_t a = 0; a < k; ++a) {
                Jtr[a] += J[i * k + a] * r0[i];
                for (size_t b = 0; b < k; ++b) JtJ[a * k + b] += J[i * k + a] * J[i * k + b];
            }
        bool improved = false;
        for (int tries = 0; tries < 20 && !improved; ++tries) {
            std::vector<double> A = JtJ, step(k);
            for (size_t a = 0; a < k; ++a) { A[a * k + a] *= 1.0 + lambda; step[a] = -Jtr[a]; }
            if (!mat_solve(A, step, static_cast<int>(k))) { lambda *= 10; continue; }
            std::vector<double> trial = theta;
            for (size_t a = 0; a < k; ++a) trial[a] += step[a];
            const double ct = chi2(trial);
            if (std::isfinite(ct) && ct <= c) {
                const double gain = c - ct;
                theta = trial; improved = true; lambda = std::max(lambda / 10, 1e-12);
                if (gain <= 1e-12 * std::max(c, 1.0)) it = 200;
                c = ct;
            } else {
                lambda *= 10;
            }
        }
        if (!improved) break;
    }
    J = jacobian(theta, r0);
    std::fill(JtJ.begin(), JtJ.end(), 0.0);
    for (size_t i = 0; i < r0.size(); ++i)
        for (size_t a = 0; a < k; ++a)
            for (size_t b = 0; b < k; ++b) JtJ[a * k + b] += J[i * k + a] * J[i * k + b];
    var_theta.assign(k, kNaN);
    if (mat_inverse_inplace(JtJ, static_cast<int>(k)))
        for (size_t a = 0; a < k; ++a) var_theta[a] = JtJ[a * k + a];
    return c;
}

} // namespace

SpeciesFactors species_factors(const std::vector<double>& dd, const std::vector<double>& da,
                               const std::vector<double>& aa, const std::vector<double>& prob,
                               int P, const FretFactors& f, const std::vector<double>& tau_d,
                               const std::vector<double>& tau_a,
                               const std::vector<double>& line_tau, const std::vector<double>& line_e,
                               double sigma_model) {
    const size_t n = dd.size();
    require_same_length(dd, da, "species_factors");
    require_same_length(dd, aa, "species_factors");
    if (P < 0 || prob.size() != n * static_cast<size_t>(P))
        throw std::invalid_argument("species_factors: probabilities must be (n_bursts, n_populations)");
    const bool life = !tau_d.empty() && !line_tau.empty();
    std::vector<double> Fdd(n), Fda(n), Faa(n), Eb(n), Sb(n);
    for (size_t b = 0; b < n; ++b) {
        Fdd[b] = dd[b] - f.bg_dd;
        Faa[b] = aa[b] - f.bg_aa;
        Fda[b] = (da[b] - f.bg_da) - f.alpha * Fdd[b] - f.delta * Faa[b];
        Eb[b] = e_of(Fda[b], Fdd[b], f.gamma);
        Sb[b] = s_of(Fda[b], Fdd[b], Faa[b], f.gamma, f.beta);
    }
    std::vector<Pop> pops(P);
    for (int s = 0; s < P; ++s) {
        Pop& p = pops[s];
        double wt = 0, st = 0, wta = 0, sta = 0, mE = 0, mS = 0;
        for (size_t b = 0; b < n; ++b) {
            const double w = prob[b * P + s];
            if (!(w > 0)) continue;
            p.n += w; p.D += w * Fdd[b]; p.A += w * Fda[b]; p.Y += w * Faa[b];
            mE += w * Eb[b]; mS += w * Sb[b];
            if (!tau_d.empty() && std::isfinite(tau_d[b])) { wt += w; st += w * tau_d[b]; }
            if (!tau_a.empty() && std::isfinite(tau_a[b])) { wta += w; sta += w * tau_a[b]; }
        }
        if (p.n <= 0) continue;
        p.D /= p.n; p.A /= p.n; p.Y /= p.n; mE /= p.n; mS /= p.n;
        double vE = 0, vS = 0, vt = 0;
        const double tm = wt > 0 ? st / wt : kNaN;
        for (size_t b = 0; b < n; ++b) {
            const double w = prob[b * P + s];
            if (!(w > 0)) continue;
            vE += w * (Eb[b] - mE) * (Eb[b] - mE);
            vS += w * (Sb[b] - mS) * (Sb[b] - mS);
            if (wt > 0 && std::isfinite(tau_d[b])) vt += w * (tau_d[b] - tm) * (tau_d[b] - tm);
        }
        p.sS = std::sqrt(vS / p.n / p.n + sigma_model * sigma_model);
        p.tau = tm;
        p.tau_a = wta > 0 ? sta / wta : kNaN;
        if (life && std::isfinite(tm)) {
            p.e_line = np_interp(tm, line_tau, line_e);
            const double slope = (np_interp(tm + 1e-3, line_tau, line_e) - np_interp(tm - 1e-3, line_tau, line_e)) / 2e-3;
            const double s_tau = std::sqrt(vt / wt / wt);
            p.sE = std::sqrt(vE / p.n / p.n + slope * slope * s_tau * s_tau + sigma_model * sigma_model);
            p.lifetime = p.e_line >= 0.05 && p.e_line <= 0.95;
        }
    }

    SpeciesFactors r;
    r.n_populations = P;
    int n_life = 0;
    for (const Pop& p : pops) n_life += p.lifetime;
    r.n_obs = P + n_life;
    r.k_species = P + 1;
    auto residuals = [&](bool species) {
        return [&, species](const std::vector<double>& t) {
            std::vector<double> out;
            const double beta = std::exp(t.back());
            for (int s = 0; s < P; ++s) {
                const Pop& p = pops[s];
                const double g = std::exp(species ? t[s] : t[0]);
                out.push_back((s_of(p.A, p.D, p.Y, g, beta) - 0.5) / p.sS);
                if (p.lifetime) out.push_back((e_of(p.A, p.D, g) - p.e_line) / p.sE);
            }
            return out;
        };
    };
    std::vector<double> th = {std::log(f.gamma), std::log(f.beta)}, var;
    r.chi2_shared = fit(residuals(false), th, var);
    r.gamma_shared = std::exp(th[0]);
    r.beta_shared = std::exp(th[1]);
    r.sigma_gamma_shared = r.gamma_shared * std::sqrt(var[0]);
    r.sigma_beta_shared = r.beta_shared * std::sqrt(var[1]);
    r.bic_shared = r.chi2_shared + r.k_shared * std::log(static_cast<double>(std::max(r.n_obs, 1)));
    r.identifiable = P >= 2 && n_life == P;
    r.bic_species = r.chi2_species = kNaN;
    r.beta_species = r.sigma_beta_species = kNaN;
    if (r.identifiable) {
        std::vector<double> ts(P + 1, th[0]);
        ts[P] = th[1];
        r.chi2_species = fit(residuals(true), ts, var);
        for (int s = 0; s < P; ++s) {
            r.gamma_species.push_back(std::exp(ts[s]));
            r.sigma_gamma_species.push_back(std::exp(ts[s]) * std::sqrt(var[s]));
        }
        r.beta_species = std::exp(ts[P]);
        r.sigma_beta_species = r.beta_species * std::sqrt(var[P]);
        r.bic_species = r.chi2_species + r.k_species * std::log(static_cast<double>(r.n_obs));
        if (r.bic_species < r.bic_shared) r.selected = "species";
    }
    const bool sp = r.selected == "species";
    r.beta = sp ? r.beta_species : f.beta;
    r.sigma_beta = sp ? r.sigma_beta_species : kNaN;
    for (int s = 0; s < P; ++s) {
        const Pop& p = pops[s];
        const double g = sp ? r.gamma_species[s] : f.gamma;
        r.gamma.push_back(g);
        r.sigma_gamma.push_back(sp ? r.sigma_gamma_species[s] : kNaN);
        r.n_eff.push_back(p.n);
        r.E_pop.push_back(e_of(p.A, p.D, g));
        r.S_pop.push_back(s_of(p.A, p.D, p.Y, g, r.beta));
        r.tau_d.push_back(p.tau);
        r.e_line.push_back(p.e_line);
        r.tau_a.push_back(p.tau_a);
    }
    r.E.resize(n);
    r.S.resize(n);
    for (size_t b = 0; b < n; ++b) {
        double w_left = 1.0, e = 0.0, s_ = 0.0;
        for (int s = 0; s < P; ++s) {
            const double w = prob[b * P + s];
            if (!(w > 0)) continue;
            w_left -= w;
            e += w * e_of(Fda[b], Fdd[b], r.gamma[s]);
            s_ += w * s_of(Fda[b], Fdd[b], Faa[b], r.gamma[s], r.beta);
        }
        if (w_left > 1e-12) {
            e += w_left * e_of(Fda[b], Fdd[b], f.gamma);
            s_ += w_left * s_of(Fda[b], Fdd[b], Faa[b], f.gamma, r.beta);
        }
        r.E[b] = e;
        r.S[b] = s_;
    }
    return r;
}

} // namespace tttrlib
