/*!
 * @file PoissonScore.h
 * @brief A Poisson count model's goodness and its derivatives: the deviance, the
 *        deviance residuals, and the gradient and information matrix a Fisher
 *        scoring (or Newton-Raphson) step needs.
 *
 * Header-only and std-only, so imp.bff carries it as a verbatim copy
 * (`include/internal/PoissonScore.h`). Moved from imp.bff's
 * `BayesianFisherScoring.h` and `FitStatistics.h` (2026-09-15, tpeulen: "poisson
 * score tttrlib - also used in fit2x"), arithmetic unchanged.
 *
 * **Relation to fit2x's statistic** (`DecayStatistics.h`): `twoIstar(C, M, N)` is
 * `-(1/N) sum C log(M/C)` over the `2N` channels, the multinomial form. When the
 * model's total equals the data's (`sum M = sum C`) the deviance's `2 sum (M - C)`
 * term vanishes and `twoIstar = poisson_deviance / (2N)` (checked in
 * `test/cpp/test_poisson_score.cpp`). The deviance keeps that term, so it is also
 * the likelihood ratio for a model whose total is free; the two stay separate
 * statistics.
 */
#ifndef TTTRLIB_POISSONSCORE_H
#define TTTRLIB_POISSONSCORE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

namespace tttrlib {

//! Which information matrix to build.
enum PoissonInformationKind {
  POISSON_INFORMATION_EXPECTED = 0,  //!< Fisher scoring: `J' diag(w/m) J`
  POISSON_INFORMATION_OBSERVED       //!< Newton-Raphson: `J' diag(w y / m^2) J`
};

/**
 * \brief `grad = J' w (y/m - 1)` and `A = J' W J` for a Poisson likelihood.
 *
 * For `log L = sum_b w_b [ y_b log m_b - m_b ]` with `m = m(theta)`, the
 * gradient is `J' w (y/m - 1)` and the two standard curvature matrices differ
 * only in `W`: the EXPECTED information puts `w/m` there and the OBSERVED one
 * `w y / m^2`. They agree in expectation because `E[y] = m`, and the choice
 * between them is Fisher scoring against Newton-Raphson (McCullagh & Nelder,
 * *Generalized Linear Models*, 2nd ed., 2.5).
 *
 * **Neither is an approximation to the objective.** The mode is where the
 * gradient vanishes either way; the choice only changes how fast the iteration
 * walks there, and which one is faster depends on how far from the mode you
 * are. Both drop the term `(y/m - 1) d2m/dtheta2`, which is what an exact
 * Newton step would restore -- and near the mode that term is small and
 * random-signed, which is why scoring converges at all.
 *
 * `w` is a per-bin weight, typically a 0/1 mask restricting the likelihood to
 * part of a histogram. Pass null for all ones.
 *
 * \param J `(n_bin, n_par)` row-major, `dm/dtheta`
 * \param grad `n_par` out
 * \param A `n_par x n_par` row-major out, the information matrix
 */
inline void poisson_score(const double* y, const double* m, const double* J,
                          const double* w, std::size_t n_bin, std::size_t n_par,
                          PoissonInformationKind kind, double* grad, double* A,
                          double floor = 1e-12, std::size_t n_threads = 1) {
  std::vector<double> u(n_bin), Wd(n_bin);
  for (std::size_t b = 0; b < n_bin; ++b) {
    const double mb = m[b] > floor ? m[b] : floor;
    const double wb = w ? w[b] : 1.0;
    u[b] = wb * (y[b] / mb - 1.0);
    Wd[b] = (kind == POISSON_INFORMATION_EXPECTED) ? wb / mb : wb * y[b] / (mb * mb);
  }
  for (std::size_t p = 0; p < n_par; ++p) {
    double s = 0.0;
    for (std::size_t b = 0; b < n_bin; ++b) s += J[b * n_par + p] * u[b];
    grad[p] = s;
  }
  //  `A = J' W J` accumulated as rank-one updates over the BINS rather than
  //  as a dot product per entry. Same arithmetic, same result -- but both
  //  reads then run along a contiguous row of `J`, where the obvious form
  //  strides by `n_par` down a column and misses cache on nearly every access.
  //  Measured at 114 parameters over 1808 bins: **19.1 ms the obvious way,
  //  1.94 ms this way**, and 2.06 ms for a blocked, threaded GEMM
  //  (`tttrlib::Mat`) doing the same product. The order is worth ten times
  //  what the library is, which is worth knowing before reaching for one.
  //  **And it partitions over the bins when asked.** Each thread owns a full
  //  `n_par x n_par` accumulator -- 104 kB at 114 parameters, which is nothing
  //  beside the Jacobian it is reading -- and the reduction is one pass at the
  //  end. Partitioning over PARAMETERS instead would have every thread stream
  //  the whole of `J`; partitioning over bins has each read a contiguous slice
  //  of it once. The serial path is kept exactly as it was, bit for bit, so a
  //  caller that does not ask for threads gets the same numbers as before.
  auto accumulate = [&](std::size_t b0, std::size_t b1, double* Aout) {
    for (std::size_t i = 0; i < n_par * n_par; ++i) Aout[i] = 0.0;
    for (std::size_t b = b0; b < b1; ++b) {
      const double* Jb = &J[b * n_par];
      const double wb = Wd[b];
      if (wb == 0.0) continue;
      for (std::size_t p = 0; p < n_par; ++p) {
        const double wj = wb * Jb[p];
        if (wj == 0.0) continue;
        double* Ap = &Aout[p * n_par];
        for (std::size_t q = p; q < n_par; ++q) Ap[q] += wj * Jb[q];
      }
    }
  };
  if (n_threads <= 1) {
    accumulate(0, n_bin, A);
  } else {
    std::vector<std::vector<double>> part(n_threads, std::vector<double>(n_par * n_par));
    std::vector<std::thread> th;
    for (std::size_t t = 0; t < n_threads; ++t)
      th.emplace_back(accumulate, n_bin * t / n_threads, n_bin * (t + 1) / n_threads,
                      part[t].data());
    for (auto& x : th) x.join();
    for (std::size_t i = 0; i < n_par * n_par; ++i) {
      double s = 0.0;
      for (std::size_t t = 0; t < n_threads; ++t) s += part[t][i];
      A[i] = s;
    }
  }
  for (std::size_t p = 0; p < n_par; ++p)
    for (std::size_t q = p + 1; q < n_par; ++q) A[q * n_par + p] = A[p * n_par + q];
}

/**
 * \brief `poisson_score` for a Jacobian whose rows come in blocks, each
 *        non-zero only on its own columns.
 *
 * A model of several histograms often has rows that depend on a subset of the
 * parameters each: a donor-only histogram does not see the distance
 * distribution, a reference dye not the donor spectrum. Rows
 * `[k * block_bins, (k + 1) * block_bins)` are then accumulated on
 * `block_columns[k]` only (sorted, unique), which is where the work is: on the
 * CBM56 global fit (136 parameters, 6 x 488 bins) half of `J' W J`. The
 * arithmetic per entry is `poisson_score`'s; `grad` and `A` are the same
 * up to rounding. `parallel_for(n, body)` runs `body(k)` for the blocks; pass a
 * serial loop, or a pool.
 */
template <typename ParallelFor>
inline void poisson_score_blocks(const double* y, const double* m, const double* J, const double* w,
                                          std::size_t n_bin, std::size_t n_par, PoissonInformationKind kind,
                                          const std::vector<std::vector<std::size_t>>& block_columns,
                                          std::size_t block_bins, double* grad, double* A, ParallelFor parallel_for,
                                          double floor = 1e-12) {
  const std::size_t nb = block_columns.size();
  std::vector<std::vector<double>> Asub(nb), gsub(nb);
  parallel_for(nb, [&](std::size_t k) {
    const std::vector<std::size_t>& L = block_columns[k];
    const std::size_t mk = L.size();
    std::vector<double>& As = Asub[k];
    std::vector<double>& gs = gsub[k];
    As.assign(mk * mk, 0.0);
    gs.assign(mk, 0.0);
    std::vector<double> row(mk);
    const std::size_t b1 = std::min(n_bin, (k + 1) * block_bins);
    for (std::size_t b = k * block_bins; b < b1; ++b) {
      const double wb = w ? w[b] : 1.0;
      if (wb == 0.0) continue;
      const double mb = m[b] > floor ? m[b] : floor;
      const double u = wb * (y[b] / mb - 1.0);
      const double Wd = (kind == POISSON_INFORMATION_EXPECTED) ? wb / mb : wb * y[b] / (mb * mb);
      const double* Jb = &J[b * n_par];
      for (std::size_t a = 0; a < mk; ++a) row[a] = Jb[L[a]];
      for (std::size_t a = 0; a < mk; ++a) {
        if (row[a] == 0.0) continue;
        gs[a] += row[a] * u;
        const double wa = Wd * row[a];
        double* Aa = &As[a * mk];
        for (std::size_t q = a; q < mk; ++q) Aa[q] += wa * row[q];
      }
    }
  });
  for (std::size_t p = 0; p < n_par; ++p) grad[p] = 0.0;
  for (std::size_t i = 0; i < n_par * n_par; ++i) A[i] = 0.0;
  for (std::size_t k = 0; k < nb; ++k) {
    const std::vector<std::size_t>& L = block_columns[k];
    const std::size_t mk = L.size();
    for (std::size_t a = 0; a < mk; ++a) {
      grad[L[a]] += gsub[k][a];
      for (std::size_t q = a; q < mk; ++q) A[L[a] * n_par + L[q]] += Asub[k][a * mk + q];
    }
  }
  //  columns sorted per block: everything landed on or above the diagonal
  for (std::size_t p = 0; p < n_par; ++p)
    for (std::size_t q = p + 1; q < n_par; ++q) A[q * n_par + p] = A[p * n_par + q];
}

/**
 * \brief Poisson deviance, `2 sum [ m - y + y log(y/m) ]`.
 *
 * **Why not chi-square.** For counting data the natural goodness-of-fit
 * statistic is the likelihood-ratio one, and dividing by an estimated variance
 * is a Gaussian approximation that fails where it matters -- in the tail,
 * where the counts are few and the long lifetimes live. Weighting by the
 * OBSERVED counts (`sigma = sqrt(y)`, Neyman) biases the fit low there;
 * weighting by the model (Pearson) does not, but is not the likelihood. The
 * deviance is the likelihood ratio against a model that fits every bin
 * exactly, so it needs no weights at all, and it is what "chi-square" should
 * mean for photon counting.
 *
 * A bin with `y = 0` contributes `2m`, which is the limit of `y log(y/m)` as
 * `y -> 0` and not a special case to be skipped.
 */
inline double poisson_deviance(const double* y, const double* m, std::size_t n) {
  double d = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double mi = (m[i] > 1e-300) ? m[i] : 1e-300;
    d += 2.0 * (mi - y[i]);
    if (y[i] > 0.0) d += 2.0 * y[i] * std::log(y[i] / mi);
  }
  return d;
}

//! Signed square roots of the per-bin deviance -- residuals whose sum of
//! squares IS the deviance, unlike `(y - m)/sqrt(y)`.
inline void deviance_residuals(const double* y, const double* m, std::size_t n, double* r) {
  for (std::size_t i = 0; i < n; ++i) {
    const double mi = (m[i] > 1e-300) ? m[i] : 1e-300;
    double d = 2.0 * (mi - y[i]);
    if (y[i] > 0.0) d += 2.0 * y[i] * std::log(y[i] / mi);
    if (d < 0.0) d = 0.0;
    r[i] = (y[i] >= mi ? 1.0 : -1.0) * std::sqrt(d);
  }
}

}  // namespace tttrlib

#endif  // TTTRLIB_POISSONSCORE_H
