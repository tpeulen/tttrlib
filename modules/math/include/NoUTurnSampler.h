/*!
 * @file NoUTurnSampler.h
 * @brief The No-U-Turn sampler: Hamiltonian Monte Carlo that picks its own trajectory length.
 *
 * Given a log density and its gradient, each transition integrates Hamiltonian dynamics by leapfrog
 * under a Euclidean metric, doubling the trajectory forwards or backwards at random until it turns
 * back on itself, and draws the next state from the whole trajectory. Implemented as Stan's
 * `base_nuts` (stan/mcmc/hmc/nuts/base_nuts.hpp):
 * - multinomial sampling within subtrees and biased progressive sampling between them;
 * - the generalised no-U-turn criterion on the summed momentum `rho`, checked across every merge
 *   and the sub-subtree boundaries (Betancourt, "A Conceptual Introduction to Hamiltonian Monte
 *   Carlo", arXiv:1701.02434, appendix A.4);
 * - a divergence when the energy error exceeds 1000;
 * - a dense metric: kinetic energy `p' S p / 2` with `S` the given inverse metric (a covariance);
 * - Stan's initial step-size search, and Nesterov dual averaging of the step size during warm-up
 *   to a target acceptance statistic (Hoffman & Gelman, JMLR 15, 1593 (2014), Algorithm 5, with
 *   Stan's constants gamma 0.05, t0 10, kappa 0.75).
 *
 * Header-only and std-only. Written 2026-09-15 for imp.bff PRD-146.
 */
#ifndef TTTRLIB_NOUTURNSAMPLER_H
#define TTTRLIB_NOUTURNSAMPLER_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace tttrlib {

//! One NUTS transition's outcome.
struct NutsTransition {
  std::vector<double> q;          //!< the new state
  double log_density = 0.0;       //!< log density there
  double accept_stat = 0.0;       //!< mean Metropolis acceptance over the trajectory (what adaptation targets)
  int tree_depth = 0;             //!< doublings made
  int n_leapfrog = 0;             //!< leapfrog steps (gradient evaluations)
  bool divergent = false;         //!< the energy error exceeded the limit
  double energy = 0.0;            //!< Hamiltonian at the new state (for E-BFMI)
};

class NoUTurnSampler {
 public:
  //! log density at q; writes its gradient into grad (resized by the callee or pre-sized to dim)
  using Density = std::function<double(const std::vector<double>& q, std::vector<double>& grad)>;

  NoUTurnSampler(std::size_t dim, Density density, std::uint64_t seed = 1)
      : n_(dim), f_(std::move(density)), rng_(seed), S_(dim * dim, 0.0), L_(dim * dim, 0.0) {
    for (std::size_t i = 0; i < n_; ++i) { S_[i * n_ + i] = 1.0; L_[i * n_ + i] = 1.0; }
  }

  //! The inverse metric (a covariance, dim x dim, row-major, positive definite). Identity by default.
  void set_inverse_metric(const std::vector<double>& cov) {
    if (cov.size() != n_ * n_) throw std::invalid_argument("NoUTurnSampler: inverse metric size");
    S_ = cov;
    L_.assign(n_ * n_, 0.0);
    for (std::size_t i = 0; i < n_; ++i)
      for (std::size_t j = 0; j <= i; ++j) {
        double s = S_[i * n_ + j];
        for (std::size_t k = 0; k < j; ++k) s -= L_[i * n_ + k] * L_[j * n_ + k];
        if (i == j) {
          if (!(s > 0.0)) throw std::invalid_argument("NoUTurnSampler: inverse metric not positive definite");
          L_[i * n_ + i] = std::sqrt(s);
        } else {
          L_[i * n_ + j] = s / L_[j * n_ + j];
        }
      }
  }

  void set_step_size(double eps) { eps_ = eps; }
  double step_size() const { return eps_; }
  void set_max_depth(int d) { max_depth_ = d; }
  void set_target_accept(double delta) { delta_ = delta; }

  //! Set the state (evaluates the density there).
  void set_state(const std::vector<double>& q) {
    q_ = q;
    grad_.assign(n_, 0.0);
    lp_ = f_(q_, grad_);
    if (!std::isfinite(lp_)) throw std::invalid_argument("NoUTurnSampler: the log density is not finite at the start");
  }
  const std::vector<double>& state() const { return q_; }

  //! Stan's heuristic: double or halve the step size until one leapfrog step's acceptance crosses 0.8.
  void init_step_size() {
    const std::vector<double> q0 = q_, g0 = grad_;
    const double lp0 = lp_;
    std::vector<double> p(n_);
    auto one_step_delta = [&]() {
      q_ = q0; grad_ = g0; lp_ = lp0;
      sample_momentum(p);
      const double H0 = hamiltonian(lp_, p);
      leapfrog(q_, p, grad_, lp_, eps_);
      double h = hamiltonian(lp_, p);
      if (std::isnan(h)) h = std::numeric_limits<double>::infinity();
      return H0 - h;
    };
    double delta_H = one_step_delta();
    const int direction = delta_H > std::log(0.8) ? 1 : -1;
    for (;;) {
      delta_H = one_step_delta();
      if (direction == 1 && !(delta_H > std::log(0.8))) break;
      if (direction == -1 && !(delta_H < std::log(0.8))) break;
      eps_ = direction == 1 ? 2.0 * eps_ : 0.5 * eps_;
      if (eps_ > 1e7) throw std::runtime_error("NoUTurnSampler: posterior is improper (step size diverged)");
      if (eps_ == 0.0) throw std::runtime_error("NoUTurnSampler: no acceptably small step size");
    }
    q_ = q0; grad_ = g0; lp_ = lp0;
  }

  //! Start step-size adaptation (dual averaging around log(10 eps)).
  void begin_adaptation() {
    mu_ = std::log(10.0 * eps_);
    s_bar_ = 0.0; x_bar_ = 0.0; counter_ = 0.0;
    adapting_ = true;
  }
  //! Stop adapting; the step size becomes the dual-averaged value.
  void end_adaptation() {
    if (adapting_ && counter_ > 0.0) eps_ = std::exp(x_bar_);
    adapting_ = false;
  }

  NutsTransition transition() {
    std::vector<double> p(n_);
    sample_momentum(p);
    State z{q_, p, grad_, lp_};
    const double H0 = hamiltonian(z.lp, z.p);
    State z_fwd = z, z_bck = z, z_sample = z, z_propose = z;
    std::vector<double> p_fwd_fwd = z.p, p_fwd_bck = z.p, p_bck_fwd = z.p, p_bck_bck = z.p;
    std::vector<double> ps_fwd_fwd = dtau_dp(z.p), ps_fwd_bck = ps_fwd_fwd, ps_bck_fwd = ps_fwd_fwd, ps_bck_bck = ps_fwd_fwd;
    std::vector<double> rho = z.p;
    double log_sum_weight = 0.0;
    int n_leapfrog = 0, depth = 0;
    double sum_metro_prob = 0.0;
    divergent_ = false;
    std::uniform_real_distribution<double> U(0.0, 1.0);
    while (depth < max_depth_) {
      std::vector<double> rho_fwd(n_, 0.0), rho_bck(n_, 0.0);
      bool valid_subtree = false;
      double log_sum_weight_subtree = -std::numeric_limits<double>::infinity();
      if (U(rng_) > 0.5) {
        rho_bck = rho; p_bck_fwd = p_fwd_bck; ps_bck_fwd = ps_fwd_bck;
        State zz = z_fwd;
        valid_subtree = build_tree(depth, zz, z_propose, ps_fwd_bck, ps_fwd_fwd, rho_fwd, p_fwd_bck, p_fwd_fwd, H0, 1.0,
                                   n_leapfrog, log_sum_weight_subtree, sum_metro_prob);
        z_fwd = zz;
      } else {
        rho_fwd = rho; p_fwd_bck = p_bck_fwd; ps_fwd_bck = ps_bck_fwd;
        State zz = z_bck;
        valid_subtree = build_tree(depth, zz, z_propose, ps_bck_fwd, ps_bck_bck, rho_bck, p_bck_fwd, p_bck_bck, H0, -1.0,
                                   n_leapfrog, log_sum_weight_subtree, sum_metro_prob);
        z_bck = zz;
      }
      if (!valid_subtree) break;
      ++depth;
      if (log_sum_weight_subtree > log_sum_weight) {
        z_sample = z_propose;
      } else {
        const double accept_prob = std::exp(log_sum_weight_subtree - log_sum_weight);
        if (U(rng_) < accept_prob) z_sample = z_propose;
      }
      log_sum_weight = log_sum_exp(log_sum_weight, log_sum_weight_subtree);
      for (std::size_t i = 0; i < n_; ++i) rho[i] = rho_bck[i] + rho_fwd[i];
      bool persist = criterion(ps_bck_bck, ps_fwd_fwd, rho);
      std::vector<double> rho_ext(n_);
      for (std::size_t i = 0; i < n_; ++i) rho_ext[i] = rho_bck[i] + p_fwd_bck[i];
      persist = persist && criterion(ps_bck_bck, ps_fwd_bck, rho_ext);
      for (std::size_t i = 0; i < n_; ++i) rho_ext[i] = rho_fwd[i] + p_bck_fwd[i];
      persist = persist && criterion(ps_bck_fwd, ps_fwd_fwd, rho_ext);
      if (!persist) break;
    }
    NutsTransition t;
    t.accept_stat = n_leapfrog > 0 ? sum_metro_prob / double(n_leapfrog) : 0.0;
    t.tree_depth = depth;
    t.n_leapfrog = n_leapfrog;
    t.divergent = divergent_;
    q_ = z_sample.q; grad_ = z_sample.grad; lp_ = z_sample.lp;
    t.q = q_; t.log_density = lp_;
    t.energy = hamiltonian(z_sample.lp, z_sample.p);
    if (adapting_) learn_step_size(t.accept_stat);
    return t;
  }

 private:
  struct State { std::vector<double> q, p, grad; double lp; };

  static double log_sum_exp(double a, double b) {
    if (a == -std::numeric_limits<double>::infinity()) return b;
    if (b == -std::numeric_limits<double>::infinity()) return a;
    const double m = std::max(a, b);
    return m + std::log(std::exp(a - m) + std::exp(b - m));
  }
  static bool criterion(const std::vector<double>& ps_minus, const std::vector<double>& ps_plus, const std::vector<double>& rho) {
    double a = 0.0, b = 0.0;
    for (std::size_t i = 0; i < rho.size(); ++i) { a += ps_plus[i] * rho[i]; b += ps_minus[i] * rho[i]; }
    return a > 0.0 && b > 0.0;
  }
  std::vector<double> dtau_dp(const std::vector<double>& p) const {
    std::vector<double> v(n_, 0.0);
    for (std::size_t i = 0; i < n_; ++i) { double s = 0.0; for (std::size_t j = 0; j < n_; ++j) s += S_[i * n_ + j] * p[j]; v[i] = s; }
    return v;
  }
  double hamiltonian(double lp, const std::vector<double>& p) const {
    const std::vector<double> v = dtau_dp(p);
    double k = 0.0;
    for (std::size_t i = 0; i < n_; ++i) k += p[i] * v[i];
    return -lp + 0.5 * k;
  }
  //! p ~ N(0, S^-1): p = L^-T u, S = L L'
  void sample_momentum(std::vector<double>& p) {
    std::normal_distribution<double> N(0.0, 1.0);
    std::vector<double> u(n_);
    for (double& v : u) v = N(rng_);
    for (std::size_t ii = n_; ii-- > 0;) {
      double s = u[ii];
      for (std::size_t k = ii + 1; k < n_; ++k) s -= L_[k * n_ + ii] * p[k];
      p[ii] = s / L_[ii * n_ + ii];
    }
  }
  void leapfrog(std::vector<double>& q, std::vector<double>& p, std::vector<double>& grad, double& lp, double eps) {
    for (std::size_t i = 0; i < n_; ++i) p[i] += 0.5 * eps * grad[i];       // grad of log density = -dphi/dq
    const std::vector<double> v = dtau_dp(p);
    for (std::size_t i = 0; i < n_; ++i) q[i] += eps * v[i];
    lp = f_(q, grad);
    if (!std::isfinite(lp)) { lp = -std::numeric_limits<double>::infinity(); return; }
    for (std::size_t i = 0; i < n_; ++i) p[i] += 0.5 * eps * grad[i];
  }
  bool build_tree(int depth, State& z, State& z_propose, std::vector<double>& ps_beg, std::vector<double>& ps_end,
                  std::vector<double>& rho, std::vector<double>& p_beg, std::vector<double>& p_end, double H0, double sign,
                  int& n_leapfrog, double& log_sum_weight, double& sum_metro_prob) {
    if (depth == 0) {
      leapfrog(z.q, z.p, z.grad, z.lp, sign * eps_);
      ++n_leapfrog;
      double h = std::isfinite(z.lp) ? hamiltonian(z.lp, z.p) : std::numeric_limits<double>::infinity();
      if (std::isnan(h)) h = std::numeric_limits<double>::infinity();
      if (h - H0 > max_delta_H_) divergent_ = true;
      log_sum_weight = log_sum_exp(log_sum_weight, H0 - h);
      sum_metro_prob += H0 - h > 0.0 ? 1.0 : std::exp(H0 - h);
      z_propose = z;
      ps_beg = dtau_dp(z.p);
      ps_end = ps_beg;
      for (std::size_t i = 0; i < n_; ++i) rho[i] += z.p[i];
      p_beg = z.p;
      p_end = p_beg;
      return !divergent_;
    }
    // the initial subtree
    double log_sum_weight_init = -std::numeric_limits<double>::infinity();
    std::vector<double> p_init_end(n_, 0.0), ps_init_end(n_, 0.0), rho_init(n_, 0.0);
    if (!build_tree(depth - 1, z, z_propose, ps_beg, ps_init_end, rho_init, p_beg, p_init_end, H0, sign, n_leapfrog,
                    log_sum_weight_init, sum_metro_prob))
      return false;
    // the final subtree
    State z_propose_final = z;
    double log_sum_weight_final = -std::numeric_limits<double>::infinity();
    std::vector<double> p_final_beg(n_, 0.0), ps_final_beg(n_, 0.0), rho_final(n_, 0.0);
    if (!build_tree(depth - 1, z, z_propose_final, ps_final_beg, ps_end, rho_final, p_final_beg, p_end, H0, sign, n_leapfrog,
                    log_sum_weight_final, sum_metro_prob))
      return false;
    // multinomial sample from the right subtree
    const double log_sum_weight_subtree = log_sum_exp(log_sum_weight_init, log_sum_weight_final);
    log_sum_weight = log_sum_exp(log_sum_weight, log_sum_weight_subtree);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    if (log_sum_weight_final > log_sum_weight_subtree) {
      z_propose = z_propose_final;
    } else {
      const double accept_prob = std::exp(log_sum_weight_final - log_sum_weight_subtree);
      if (U(rng_) < accept_prob) z_propose = z_propose_final;
    }
    std::vector<double> rho_subtree(n_);
    for (std::size_t i = 0; i < n_; ++i) { rho_subtree[i] = rho_init[i] + rho_final[i]; rho[i] += rho_subtree[i]; }
    bool persist = criterion(ps_beg, ps_end, rho_subtree);
    std::vector<double> rho_ext(n_);
    for (std::size_t i = 0; i < n_; ++i) rho_ext[i] = rho_init[i] + p_final_beg[i];
    persist = persist && criterion(ps_beg, ps_final_beg, rho_ext);
    for (std::size_t i = 0; i < n_; ++i) rho_ext[i] = rho_final[i] + p_init_end[i];
    persist = persist && criterion(ps_init_end, ps_end, rho_ext);
    return persist;
  }
  void learn_step_size(double adapt_stat) {
    counter_ += 1.0;
    adapt_stat = std::min(1.0, adapt_stat);
    const double eta = 1.0 / (counter_ + t0_);
    s_bar_ = (1.0 - eta) * s_bar_ + eta * (delta_ - adapt_stat);
    const double x = mu_ - s_bar_ * std::sqrt(counter_) / gamma_;
    const double x_eta = std::pow(counter_, -kappa_);
    x_bar_ = (1.0 - x_eta) * x_bar_ + x_eta * x;
    eps_ = std::exp(x);
  }

  std::size_t n_;
  Density f_;
  std::mt19937_64 rng_;
  std::vector<double> S_, L_;          // inverse metric and its Cholesky factor
  std::vector<double> q_, grad_;
  double lp_ = 0.0;
  double eps_ = 1.0;
  int max_depth_ = 10;
  double max_delta_H_ = 1000.0;
  bool divergent_ = false;
  // dual averaging
  bool adapting_ = false;
  double delta_ = 0.8, gamma_ = 0.05, t0_ = 10.0, kappa_ = 0.75;
  double mu_ = 0.0, s_bar_ = 0.0, x_bar_ = 0.0, counter_ = 0.0;
};

}  // namespace tttrlib

#endif  // TTTRLIB_NOUTURNSAMPLER_H
