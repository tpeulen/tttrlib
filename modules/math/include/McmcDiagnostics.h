/*!
 * @file McmcDiagnostics.h
 * @brief Convergence diagnostics for Markov chain Monte Carlo draws: rank-normalised split
 *        R-hat (bulk and tail), effective sample sizes (bulk, tail, mean, quantile) and the
 *        Monte Carlo standard error of the mean.
 *
 * Vehtari, Gelman, Simpson, Carpenter & Buerkner, "Rank-normalization, folding, and
 * localization: an improved R-hat for assessing convergence of MCMC", Bayesian Analysis 16,
 * 667 (2021) -- computed as arviz (arviz_stats 1.3, `base/diagnostics.py`) and Stan compute it:
 * - Chains are split in halves (first and last floor(n/2) draws).
 * - Ranks are averaged over ties and back-transformed with Blom's offset 3/8,
 *   `(r - 3/8) / (N + 1/4)`, through the normal quantile function (Wichura, AS241, Appl.
 *   Statist. 37, 477 (1988)).
 * - Bulk R-hat is taken on the rank-normalised draws, tail R-hat on the rank-normalised
 *   |x - median|, and R-hat = max(bulk, tail).
 * - ESS combines the chains' autocovariances with the between-chain variance (Stan's
 *   estimator) and truncates the autocorrelation sum by Geyer's initial positive and initial
 *   monotone sequences.
 * - Tail ESS = min over the 5 % and 95 % quantile indicators (numpy's linear quantile).
 * The autocovariance is computed lag by lag, and only as far as the Geyer sequence needs,
 * instead of by FFT; the values are the same to rounding.
 *
 * Header-only and std-only. Chains are `std::vector<std::vector<double>>`, one row per chain,
 * all of equal length. Written 2026-09-15 for imp.bff PRD-145; checked against arviz there.
 */
#ifndef TTTRLIB_MCMCDIAGNOSTICS_H
#define TTTRLIB_MCMCDIAGNOSTICS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace tttrlib {

using McmcChains = std::vector<std::vector<double>>;

//! The standard normal quantile function, Wichura's AS241 (PPND16, about 16 digits).
inline double normal_quantile(double p) {
  if (!(p > 0.0 && p < 1.0)) {
    if (p == 0.0) return -std::numeric_limits<double>::infinity();
    if (p == 1.0) return std::numeric_limits<double>::infinity();
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double q = p - 0.5;
  if (std::fabs(q) <= 0.425) {
    const double r = 0.180625 - q * q;
    return q * (((((((2509.0809287301226727 * r + 33430.575583588128105) * r + 67265.770927008700853) * r +
                    45921.953931549871457) * r + 13731.693765509461125) * r + 1971.5909503065514427) * r +
                 133.14166789178437745) * r + 3.387132872796366608) /
           (((((((5226.495278852545925 * r + 28729.085735721942674) * r + 39307.89580009271061) * r +
                21213.794301586595867) * r + 5394.1960214247511077) * r + 687.1870074920579083) * r +
             42.313330701600911252) * r + 1.0);
  }
  double r = q < 0.0 ? p : 1.0 - p;
  r = std::sqrt(-std::log(r));
  double val;
  if (r <= 5.0) {
    r -= 1.6;
    val = (((((((7.7454501427834140764e-4 * r + 0.0227238449892691845833) * r + 0.24178072517745061177) * r +
               1.27045825245236838258) * r + 3.64784832476320460504) * r + 5.7694972214606914055) * r +
            4.6303378461565452959) * r + 1.42343711074968357734) /
          (((((((1.05075007164441684324e-9 * r + 5.475938084995344946e-4) * r + 0.0151986665636164571966) * r +
               0.14810397642748007459) * r + 0.68976733498510000455) * r + 1.6763848301838038494) * r +
            2.05319162663775882187) * r + 1.0);
  } else {
    r -= 5.0;
    val = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r + 0.0012426609473880784386) * r +
               0.026532189526576123093) * r + 0.29656057182850489123) * r + 1.7848265399172913358) * r +
            5.4637849111641143699) * r + 6.6579046435011037772) /
          (((((((2.04426310338993978564e-15 * r + 1.4215117583164458887e-7) * r + 1.8463183175100546818e-5) * r +
               7.868691311456732598e-4) * r + 0.0148753612908506148525) * r + 0.13692988092273580531) * r +
            0.59983220655588793769) * r + 1.0);
  }
  return q < 0.0 ? -val : val;
}

namespace mcmc_detail {

inline void check(const McmcChains& x) {
  if (x.empty() || x[0].empty()) throw std::invalid_argument("mcmc diagnostics: no draws");
  for (const auto& c : x)
    if (c.size() != x[0].size()) throw std::invalid_argument("mcmc diagnostics: chains of unequal length");
}

//! first and last floor(n/2) draws of every chain, as separate chains
inline McmcChains split(const McmcChains& x) {
  const std::size_t n = x[0].size(), h = n / 2;
  McmcChains out;
  out.reserve(2 * x.size());
  for (const auto& c : x) out.emplace_back(c.begin(), c.begin() + std::ptrdiff_t(h));
  for (const auto& c : x) out.emplace_back(c.end() - std::ptrdiff_t(h), c.end());
  return out;
}

inline std::vector<double> pooled(const McmcChains& x) {
  std::vector<double> v;
  for (const auto& c : x) v.insert(v.end(), c.begin(), c.end());
  return v;
}

//! numpy's median of all draws
inline double median(const McmcChains& x) {
  std::vector<double> v = pooled(x);
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

//! numpy's linear (type 7) quantile of all draws
inline double quantile(const McmcChains& x, double prob) {
  std::vector<double> v = pooled(x);
  std::sort(v.begin(), v.end());
  const double h = (double(v.size()) - 1.0) * prob;
  const std::size_t lo = std::size_t(std::floor(h));
  if (lo + 1 >= v.size()) return v.back();
  return v[lo] + (h - double(lo)) * (v[lo + 1] - v[lo]);
}

//! rank-normalisation of all draws together: average ranks, Blom's offset, normal quantiles
inline McmcChains z_scale(const McmcChains& x) {
  const std::size_t m = x.size(), n = x[0].size(), N = m * n;
  std::vector<double> v = pooled(x);
  std::vector<std::size_t> idx(N);
  std::iota(idx.begin(), idx.end(), std::size_t(0));
  std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return v[a] < v[b]; });
  std::vector<double> rank(N);
  for (std::size_t i = 0; i < N;) {
    std::size_t j = i;
    while (j + 1 < N && v[idx[j + 1]] == v[idx[i]]) ++j;
    const double r = 0.5 * (double(i + 1) + double(j + 1));
    for (std::size_t k = i; k <= j; ++k) rank[idx[k]] = r;
    i = j + 1;
  }
  const double c = 3.0 / 8.0;
  McmcChains out(m, std::vector<double>(n));
  for (std::size_t a = 0; a < m; ++a)
    for (std::size_t i = 0; i < n; ++i) out[a][i] = normal_quantile((rank[a * n + i] - c) / (double(N) - 2.0 * c + 1.0));
  return out;
}

//! the classic potential scale reduction on (already split) chains
inline double rhat(const McmcChains& x) {
  const std::size_t m = x.size(), n = x[0].size();
  std::vector<double> mean(m, 0.0), var(m, 0.0);
  for (std::size_t a = 0; a < m; ++a) {
    for (double v : x[a]) mean[a] += v;
    mean[a] /= double(n);
    for (double v : x[a]) var[a] += (v - mean[a]) * (v - mean[a]);
    var[a] /= double(n - 1);
  }
  double mm = 0.0;
  for (double v : mean) mm += v;
  mm /= double(m);
  double vm = 0.0;
  for (double v : mean) vm += (v - mm) * (v - mm);
  vm /= double(m - 1);
  const double B = double(n) * vm;
  double W = 0.0;
  for (double v : var) W += v;
  W /= double(m);
  return std::sqrt((B / W + double(n) - 1.0) / double(n));
}

//! arviz's `_ess` on (already split / transformed) chains
inline double ess(const McmcChains& x) {
  const std::size_t m = x.size(), n = x[0].size();
  double lo = std::numeric_limits<double>::infinity(), hi = -lo;
  for (const auto& c : x) for (double v : c) { lo = std::min(lo, v); hi = std::max(hi, v); }
  if (hi - lo < 1e-15) return double(m * n);
  std::vector<std::vector<double>> centred(m, std::vector<double>(n));
  std::vector<double> chain_mean(m, 0.0);
  for (std::size_t a = 0; a < m; ++a) {
    for (double v : x[a]) chain_mean[a] += v;
    chain_mean[a] /= double(n);
    for (std::size_t i = 0; i < n; ++i) centred[a][i] = x[a][i] - chain_mean[a];
  }
  std::vector<double> mean_acov;                       //: mean over chains of the lag-t autocovariance, filled on demand
  auto acov = [&](std::size_t t) {
    while (mean_acov.size() <= t) {
      const std::size_t k = mean_acov.size();
      double s = 0.0;
      for (std::size_t a = 0; a < m; ++a) {
        double sa = 0.0;
        for (std::size_t i = 0; i + k < n; ++i) sa += centred[a][i] * centred[a][i + k];
        s += sa / double(n);
      }
      mean_acov.push_back(s / double(m));
    }
    return mean_acov[t];
  };
  const double mean_var = acov(0) * double(n) / (double(n) - 1.0);
  double var_plus = mean_var * (double(n) - 1.0) / double(n);
  if (m > 1) {
    double mm = 0.0;
    for (double v : chain_mean) mm += v;
    mm /= double(m);
    double vm = 0.0;
    for (double v : chain_mean) vm += (v - mm) * (v - mm);
    var_plus += vm / double(m - 1);
  }
  std::vector<double> rho(n, 0.0);
  double rho_even = 1.0;
  rho[0] = rho_even;
  double rho_odd = 1.0 - (mean_var - acov(1)) / var_plus;
  rho[1] = rho_odd;
  std::size_t t = 1;
  while (t < n - 3 && (rho_even + rho_odd) > 0.0) {       //: Geyer's initial positive sequence
    rho_even = 1.0 - (mean_var - acov(t + 1)) / var_plus;
    rho_odd = 1.0 - (mean_var - acov(t + 2)) / var_plus;
    if (rho_even + rho_odd >= 0.0) { rho[t + 1] = rho_even; rho[t + 2] = rho_odd; }
    t += 2;
  }
  const std::ptrdiff_t max_t = std::ptrdiff_t(t) - 2;
  if (rho_even > 0.0) rho[std::size_t(max_t + 1)] = rho_even;
  for (std::ptrdiff_t u = 1; u <= max_t - 2; u += 2) {  //: Geyer's initial monotone sequence
    if (rho[std::size_t(u + 1)] + rho[std::size_t(u + 2)] > rho[std::size_t(u - 1)] + rho[std::size_t(u)]) {
      rho[std::size_t(u + 1)] = 0.5 * (rho[std::size_t(u - 1)] + rho[std::size_t(u)]);
      rho[std::size_t(u + 2)] = rho[std::size_t(u + 1)];
    }
  }
  const double total = double(m * n);
  double tau = -1.0;
  for (std::ptrdiff_t u = 0; u <= max_t; ++u) tau += 2.0 * rho[std::size_t(u)];
  if (max_t + 1 < std::ptrdiff_t(n)) tau += rho[std::size_t(max_t + 1)];
  tau = std::max(tau, 1.0 / std::log10(total));
  return total / tau;
}

}  // namespace mcmc_detail

//! Rank-normalised split R-hat: max of the bulk and the tail (folded) value. Needs >= 2 chains, >= 4 draws.
inline double rhat_rank(const McmcChains& x) {
  mcmc_detail::check(x);
  if (x.size() < 2 || x[0].size() < 4) return std::numeric_limits<double>::quiet_NaN();
  const McmcChains s = mcmc_detail::split(x);
  const double bulk = mcmc_detail::rhat(mcmc_detail::z_scale(s));
  const double med = mcmc_detail::median(s);
  McmcChains f = s;
  for (auto& c : f) for (double& v : c) v = std::fabs(v - med);
  const double tail = mcmc_detail::rhat(mcmc_detail::z_scale(f));
  return std::max(bulk, tail);
}

//! Bulk effective sample size: ESS of the rank-normalised split chains.
inline double ess_bulk(const McmcChains& x) {
  mcmc_detail::check(x);
  if (x[0].size() < 4) return std::numeric_limits<double>::quiet_NaN();
  return mcmc_detail::ess(mcmc_detail::z_scale(mcmc_detail::split(x)));
}

//! ESS of the indicator draws <= the `prob` quantile of all draws.
inline double ess_quantile(const McmcChains& x, double prob) {
  mcmc_detail::check(x);
  if (x[0].size() < 4) return std::numeric_limits<double>::quiet_NaN();
  const double q = mcmc_detail::quantile(x, prob);
  McmcChains ind = x;
  for (auto& c : ind) for (double& v : c) v = v <= q ? 1.0 : 0.0;
  return mcmc_detail::ess(mcmc_detail::split(ind));
}

//! Tail effective sample size: the smaller of the ESS at the `prob` and `1 - prob` quantiles (0.05 by default).
inline double ess_tail(const McmcChains& x, double prob = 0.05) {
  const double p = std::min(prob, 1.0 - prob);
  return std::min(ess_quantile(x, p), ess_quantile(x, 1.0 - p));
}

//! ESS of the mean: the split chains untransformed.
inline double ess_mean(const McmcChains& x) {
  mcmc_detail::check(x);
  if (x[0].size() < 4) return std::numeric_limits<double>::quiet_NaN();
  return mcmc_detail::ess(mcmc_detail::split(x));
}

//! Monte Carlo standard error of the mean: sd (ddof 1, all draws) / sqrt(ess_mean).
inline double mcse_mean(const McmcChains& x) {
  const std::vector<double> v = mcmc_detail::pooled(x);
  double mean = 0.0;
  for (double a : v) mean += a;
  mean /= double(v.size());
  double s2 = 0.0;
  for (double a : v) s2 += (a - mean) * (a - mean);
  s2 /= double(v.size() - 1);
  return std::sqrt(s2) / std::sqrt(ess_mean(x));
}

}  // namespace tttrlib

#endif  // TTTRLIB_MCMCDIAGNOSTICS_H
