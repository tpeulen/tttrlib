// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_LATTICEDIFFUSION_H
#define TTTRLIB_LATTICEDIFFUSION_H

// Validation: A/B-TESTED 2026-08-19 -- adjoint vs central differences of the forward (the dot-product
//   identity <dL/dtheta, v> == d/dh L(theta + h v)) for d, decay and the initial density, both flux forms,
//   checkpoint lengths that do and do not divide n_steps, and a domain touching the shell: 1e-8..1e-12 rel.
//   test/cpp/test_lattice_diffusion.cpp. The forward is the kernel imp.bff validated against analytic
//   diffusion (its test/quenching/test_quenching_field.py); imp.bff vendors this header verbatim.
//   2026-10-20: LATTICE_FLUX_DETAILED_BALANCE gets the same adjoint identity (1e-7..1e-12) plus a stationary
//   weight field to 3e-16 over 500 steps, relaxation onto it from any start, and w = 1 equal to Smoluchowski.
//   Register: okf/testing/math-kernel-validation.md

// Explicit propagation of a density on a masked cubic lattice, and its adjoint.
//
//   dp/dt = div(D grad p) - k p        on the voxels where `bounds` is non-zero
//
// discretised as one explicit Euler sweep of the 7-point stencil per step:
//
//   p'_c = (p_c - sum_m flux_cm) * decay_c,      c interior and in the domain
//
// with `d` already carrying D dt/dg^2 and `decay` = exp(-k dt) -- the rate as
// a *factor*, the exact solution of dp/dt = -k p over the step, so it adds no
// stability constraint (subtracting k dt diverges once k dt > 1). Two forms of
// the flux between voxel c and its neighbour m, both dropping the term when
// `bounds[m]` is zero (a reflecting wall):
//
//   LATTICE_FLUX_SMOLUCHOWSKI   0.5 (d_c + d_m) (p_c - p_m) b_m   equilibrium uniform, whatever D
//   LATTICE_FLUX_ITO            (d_c p_c - d_m p_m) b_m           equilibrium p ~ 1/D
//   LATTICE_FLUX_DETAILED_BALANCE
//       0.5 (d_c + d_m) sqrt(w_c w_m) (p_c / w_c - p_m / w_m) b_m      equilibrium p ~ w, whatever D
//
// The third is the Smoluchowski equation *with a potential*, w = exp(-U/kT)
// the equilibrium weight. Mobility and equilibrium are then separate inputs:
// the flux vanishes exactly at p = w on every edge (detailed balance, so the
// discrete scheme's stationary state is w itself, not an approximation of it),
// and the arithmetic-mean mobility sets only how fast it is reached. With
// w = 1 it is LATTICE_FLUX_SMOLUCHOWSKI term for term. The geometric mean
// sqrt(w_c w_m) is what makes the generator symmetric under u = p / sqrt(w):
// off-diagonal -0.5 (d_c + d_m) b_m, the same as the plain Smoluchowski form,
// diagonal sum_m 0.5 (d_c + d_m) b_m sqrt(w_m / w_c). The weights enter as
// r = sqrt(w), computed once by lattice_root_weight().
//
// The outer shell of the grid is written to zero rather than left alone: the
// stencil cannot be evaluated there, and with ping-pong buffers those voxels
// would otherwise hold the state from two steps ago -- stale data that never
// decays and never diffuses, silently added into every population sum.
//
// The adjoint. One sweep is linear in the density, p_{n+1} = A p_n, with A
// built from d, decay and bounds only. So the adjoint density
// pbar_n = dL/dp_n obeys pbar_n = A^T pbar_{n+1} (plus dL/dF_k on every voxel
// at a reported step, since the observable is a plain sum), and the
// parameter gradients dL/dd, dL/ddecay are local products of the forward
// state and the adjoint at each step: the transposed stencil, run backwards.
// No tape, no autodiff library. The forward keeps no history, so the adjoint
// checkpoints every ceil(sqrt(n_steps)) steps and re-runs each segment
// forward on the way back: about twice the forward's work, two segments of
// memory instead of n_steps * ng^3 doubles. Measured 4.4x one forward on a
// 41^3 grid (imp.bff okf/validation/diffusion_adjoint.md): one checkpointed
// forward, one re-run, and a memory-bound reverse sweep at ~2.5x a forward
// sweep. Against finite differences -- one forward *per parameter* -- that is
// what makes a field, or a network's weights, fittable at all.
//
// Header-only and std-only on purpose: imp.bff (whose dye
// quenching model is the first consumer) carries a verbatim copy under its
// internal/ directory, so a `#pragma omp` is the only thing beyond the
// standard library here, and it compiles to nothing without -fopenmp.
// Everything is a flat std::vector<double> of length ng^3, row-major
// (ix*ng + iy)*ng + iz.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#ifndef TTTRLIB_LATTICE_NAMESPACE
#define TTTRLIB_LATTICE_NAMESPACE tttrlib
#endif

namespace TTTRLIB_LATTICE_NAMESPACE {

/// How the flux between two voxels is discretised; the two differ exactly
/// when the mobility varies in space, and the difference decides where the
/// density sits at equilibrium (see the header comment).
enum LatticeFluxForm {
    LATTICE_FLUX_SMOLUCHOWSKI = 0,
    LATTICE_FLUX_ITO = 1,
    LATTICE_FLUX_DETAILED_BALANCE = 2
};

/// `sqrt(w)` where `bounds` is non-zero, 1 elsewhere: what the detailed-balance
/// flux reads. Empty `weight` means w = 1 (the plain Smoluchowski form).
/*!
    \throws std::invalid_argument if `weight` is not ng^3 long, or is not
            positive and finite somewhere in the domain -- a zero weight is a
            voxel the dye can never occupy, which is a bound, not a weight.
*/
inline std::vector<double> lattice_root_weight(const std::vector<double>& weight,
                                               const std::vector<double>& bounds) {
    std::vector<double> r(bounds.size(), 1.0);
    if (weight.empty()) return r;
    if (weight.size() != bounds.size())
        throw std::invalid_argument("lattice_root_weight: weight must have as many voxels as bounds");
    for (std::size_t c = 0; c < bounds.size(); ++c) {
        if (bounds[c] == 0.0) continue;
        const double w = weight[c];
        if (!(w > 0.0) || !std::isfinite(w))
            throw std::invalid_argument("lattice_root_weight: the equilibrium weight must be positive and "
                                        "finite inside the domain");
        r[c] = std::sqrt(w);
    }
    return r;
}

/// One explicit sweep of `dp/dt = div(D grad p) - k p`, writing into `nxt`.
/*!
    The outer loop runs over x-slabs, so each thread writes a disjoint region
    and no synchronisation is needed. The outer shell is left at zero: the
    7-point stencil cannot be evaluated there, and leaving it *alone* instead
    would keep two-steps-ago state alive in a ping-pong buffer -- stale data
    that never decays and never diffuses, silently added into every population
    sum.
*/
inline void lattice_sweep(const std::vector<double>& cur, const std::vector<double>& d,
           const std::vector<double>& decay, const std::vector<double>& bounds,
           std::size_t n, bool smoluchowski, std::vector<double>& nxt) {
    std::fill(nxt.begin(), nxt.end(), 0.0);
    const long ngl = static_cast<long>(n);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long ixl = 1; ixl < ngl - 1; ++ixl) {
        const std::size_t ix = static_cast<std::size_t>(ixl);
        for (std::size_t iy = 1; iy + 1 < n; ++iy) {
            for (std::size_t iz = 1; iz + 1 < n; ++iz) {
                const std::size_t c = (ix * n + iy) * n + iz;
                if (bounds[c] == 0.0) continue;
                const double p0 = cur[c];
                const double d0 = d[c];
                const std::size_t nb[6] = {
                    ((ix - 1) * n + iy) * n + iz, ((ix + 1) * n + iy) * n + iz,
                    (ix * n + iy - 1) * n + iz,   (ix * n + iy + 1) * n + iz,
                    (ix * n + iy) * n + iz - 1,   (ix * n + iy) * n + iz + 1};
                double flux = 0.0;
                for (int q = 0; q < 6; ++q) {
                    const std::size_t m = nb[q];
                    // A zero bound is a reflecting wall: the dye cannot enter
                    // the protein, so flux to that neighbour is dropped.
                    if (smoluchowski) {
                        flux += 0.5 * (d0 + d[m]) * (p0 - cur[m]) * bounds[m];
                    } else {
                        flux += (d0 * p0 - d[m] * cur[m]) * bounds[m];
                    }
                }
                // The rate is applied as a factor: exp(-k dt) is the exact
                // solution of dp/dt = -k p over the step, so it contributes no
                // stability constraint. Subtracting k dt diverges once k dt > 1.
                nxt[c] = (p0 - flux) * decay[c];
            }
        }
    }
}

/// One explicit sweep with the detailed-balance flux, writing into `nxt`.
/*!
    `p'_c = decay_c [p_c - sum_m h_cm (r_m/r_c p_c - r_c/r_m p_m)]`,
    `h_cm = 0.5 (d_c + d_m) b_m`, `r = sqrt(w)` from lattice_root_weight().
    At `p = w` every bracket is zero, so with `decay = 1` the weight field is
    a fixed point of the *discrete* step to roundoff. Same threading and shell
    handling as lattice_sweep().
*/
inline void lattice_sweep_weighted(const std::vector<double>& cur, const std::vector<double>& d,
           const std::vector<double>& decay, const std::vector<double>& bounds,
           const std::vector<double>& root_w, std::size_t n, std::vector<double>& nxt) {
    std::fill(nxt.begin(), nxt.end(), 0.0);
    const long ngl = static_cast<long>(n);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long ixl = 1; ixl < ngl - 1; ++ixl) {
        const std::size_t ix = static_cast<std::size_t>(ixl);
        for (std::size_t iy = 1; iy + 1 < n; ++iy) {
            for (std::size_t iz = 1; iz + 1 < n; ++iz) {
                const std::size_t c = (ix * n + iy) * n + iz;
                if (bounds[c] == 0.0) continue;
                const double p0 = cur[c], d0 = d[c], r0 = root_w[c];
                const double inv0 = 1.0 / r0;
                const std::size_t nb[6] = {
                    ((ix - 1) * n + iy) * n + iz, ((ix + 1) * n + iy) * n + iz,
                    (ix * n + iy - 1) * n + iz,   (ix * n + iy + 1) * n + iz,
                    (ix * n + iy) * n + iz - 1,   (ix * n + iy) * n + iz + 1};
                double flux = 0.0;
                for (int q = 0; q < 6; ++q) {
                    const std::size_t m = nb[q];
                    const double bm = bounds[m];
                    if (bm == 0.0) continue;     // reflecting wall
                    const double h = 0.5 * (d0 + d[m]) * bm;
                    const double rm = root_w[m];
                    flux += h * (rm * inv0 * p0 - r0 / rm * cur[m]);
                }
                nxt[c] = (p0 - flux) * decay[c];
            }
        }
    }
}

inline double lattice_population(const std::vector<double>& p) {
    double total = 0.0;
    for (double v : p) total += v;
    return total;
}

/// One adjoint sweep: pbar_prev = A^T pbar, and the parameter gradients.
/*!
    Written as a *gather* over every voxel m, in one pass over its six
    neighbours: the contribution to \f$\bar p_m\f$ from m itself (the diagonal
    of A, if m is active -- interior and in the domain) and from each active
    neighbour c whose flux read \f$p_m\f$ (the off-diagonal), plus the parameter
    gradients, all from the same reads. Each output is owned by one iteration,
    so the loop parallelises like the forward one, and nothing is tabulated:
    the sweep is memory-bound, and a table is more memory. `cur` is the
    forward state the sweep was applied to (\f$p_n\f$), `pbar` the adjoint of
    its result (\f$\bar p_{n+1}\f$).

    Smoluchowski: \f$p'_c = \delta_c\,[\,p_c (1 - \sum_m w_{cm}) + \sum_m w_{cm} p_m\,]\f$
    with \f$w_{cm} = \tfrac12 (d_c + d_m) b_m\f$. Ito:
    \f$p'_c = \delta_c\,[\,p_c (1 - d_c \sum_m b_m) + \sum_m d_m b_m p_m\,]\f$.
    Both for interior c with \f$b_c \ne 0\f$; every other row of A is zero.
    Templated on the flux form so the inner loop carries no branch. A shell
    voxel has a zero row but, if its bound is non-zero, was read by an active
    neighbour -- the Python wrapper forbids that domain, the kernel stays exact.
*/
template <bool kSmoluchowski>
inline void lattice_adjoint_sweep(const std::vector<double>& cur, const std::vector<double>& d,
                   const std::vector<double>& decay, const std::vector<double>& bounds,
                   const std::vector<double>& pbar, std::size_t n,
                   std::vector<double>& pbar_prev, std::vector<double>& dbar,
                   std::vector<double>& decaybar) {
    const long ngl = static_cast<long>(n);
    const long stride[6] = {-ngl * ngl, ngl * ngl, -ngl, ngl, -1, 1};
    const double* P = cur.data();
    const double* D = d.data();
    const double* B = bounds.data();
    const double* DEC = decay.data();
    const double* PB = pbar.data();
    double* PBP = pbar_prev.data();
    double* DB = dbar.data();
    double* DECB = decaybar.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long ixl = 0; ixl < ngl; ++ixl) {
        for (long iy = 0; iy < ngl; ++iy) {
            for (long iz = 0; iz < ngl; ++iz) {
                const long m = (ixl * ngl + iy) * ngl + iz;
                const double bm = B[m];
                if (bm == 0.0) { PBP[m] = 0.0; continue; }  // out of the domain: zero row of A, and never read through a non-zero bound
                const bool xi = ixl >= 1 && ixl < ngl - 1, yi = iy >= 1 && iy < ngl - 1, zi = iz >= 1 && iz < ngl - 1;
                const bool m_interior = xi && yi && zi;
                const bool m_active = m_interior && bm != 0.0;
                // Is neighbour q interior (so possibly active)? Only the two
                // outermost layers can fail this.
                const bool k_int[6] = {ixl >= 2 && yi && zi, ixl < ngl - 2 && yi && zi,
                                       xi && iy >= 2 && zi, xi && iy < ngl - 2 && zi,
                                       xi && yi && iz >= 2, xi && yi && iz < ngl - 2};
                const double p0 = P[m], d0 = D[m], dec0 = DEC[m], pb0 = PB[m];
                double coef = 0.0, flux = 0.0, ddiag = 0.0;   // the diagonal (m active)
                double pb_off = 0.0, db_off = 0.0;             // gathered from active neighbours
                for (int q = 0; q < 6; ++q) {
                    if (!m_interior && !k_int[q]) continue;    // neither m's row nor k's exists
                    const long k = m + stride[q];
                    const double pk = P[k], dk = D[k], bk = B[k];
                    if (m_active) {
                        if (kSmoluchowski) {
                            const double w = 0.5 * (d0 + dk) * bk;
                            coef += w;
                            const double dp = p0 - pk;
                            flux += w * dp;
                            ddiag += 0.5 * dp * bk;
                        } else {
                            coef += d0 * bk;
                            flux += (d0 * p0 - dk * pk) * bk;
                            ddiag += p0 * bk;
                        }
                    }
                    if (k_int[q] && bk != 0.0) {   // k is active: it read p_m and d_m through b_m
                        const double pbk = PB[k], deck = DEC[k];
                        if (kSmoluchowski) {
                            pb_off += deck * 0.5 * (dk + d0) * bm * pbk;
                            db_off += -deck * pbk * 0.5 * (pk - p0) * bm;
                        } else {
                            pb_off += deck * d0 * bm * pbk;
                            db_off += deck * pbk * p0 * bm;
                        }
                    }
                }
                double pb = pb_off, db = db_off;
                if (m_active) {
                    pb += dec0 * (1.0 - coef) * pb0;
                    DECB[m] += pb0 * (p0 - flux);
                    db += -dec0 * pb0 * ddiag;
                }
                PBP[m] = pb;
                DB[m] += db;
            }
        }
    }
}

/// The adjoint of lattice_sweep_weighted(), in the same gather form.
/*!
    Row c of A (c active): diagonal `delta_c (1 - sum_m h_cm s_cm)`,
    off-diagonal `delta_c h_cm / s_cm`, with `s_cm = r_m / r_c`. The gradient
    is with respect to `d` and `decay`, the weights held fixed.
*/
inline void lattice_adjoint_sweep_weighted(const std::vector<double>& cur, const std::vector<double>& d,
                   const std::vector<double>& decay, const std::vector<double>& bounds,
                   const std::vector<double>& root_w,
                   const std::vector<double>& pbar, std::size_t n,
                   std::vector<double>& pbar_prev, std::vector<double>& dbar,
                   std::vector<double>& decaybar) {
    const long ngl = static_cast<long>(n);
    const long stride[6] = {-ngl * ngl, ngl * ngl, -ngl, ngl, -1, 1};
    const double* P = cur.data();
    const double* D = d.data();
    const double* B = bounds.data();
    const double* R = root_w.data();
    const double* DEC = decay.data();
    const double* PB = pbar.data();
    double* PBP = pbar_prev.data();
    double* DB = dbar.data();
    double* DECB = decaybar.data();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (long ixl = 0; ixl < ngl; ++ixl) {
        for (long iy = 0; iy < ngl; ++iy) {
            for (long iz = 0; iz < ngl; ++iz) {
                const long m = (ixl * ngl + iy) * ngl + iz;
                const double bm = B[m];
                if (bm == 0.0) { PBP[m] = 0.0; continue; }
                const bool xi = ixl >= 1 && ixl < ngl - 1, yi = iy >= 1 && iy < ngl - 1, zi = iz >= 1 && iz < ngl - 1;
                const bool m_interior = xi && yi && zi;
                const bool m_active = m_interior && bm != 0.0;
                const bool k_int[6] = {ixl >= 2 && yi && zi, ixl < ngl - 2 && yi && zi,
                                       xi && iy >= 2 && zi, xi && iy < ngl - 2 && zi,
                                       xi && yi && iz >= 2, xi && yi && iz < ngl - 2};
                const double p0 = P[m], d0 = D[m], dec0 = DEC[m], pb0 = PB[m], r0 = R[m];
                double coef = 0.0, flux = 0.0, ddiag = 0.0;
                double pb_off = 0.0, db_off = 0.0;
                for (int q = 0; q < 6; ++q) {
                    if (!m_interior && !k_int[q]) continue;
                    const long k = m + stride[q];
                    const double bk = B[k];
                    if (bk == 0.0) continue;     // no edge either way: h vanishes in both rows
                    const double pk = P[k], dk = D[k], rk = R[k];
                    const double up = rk / r0, dn = r0 / rk;   // s_mk and 1/s_mk
                    if (m_active) {
                        const double h = 0.5 * (d0 + dk) * bk;
                        const double j = up * p0 - dn * pk;
                        coef += h * up;
                        flux += h * j;
                        ddiag += 0.5 * bk * j;
                    }
                    if (k_int[q]) {               // k active: row k read p_m and d_m through b_m
                        const double pbk = PB[k], deck = DEC[k];
                        pb_off += deck * 0.5 * (dk + d0) * bm * up * pbk;          // h_km / s_km, s_km = r_m / r_k
                        db_off += -deck * pbk * 0.5 * bm * (dn * pk - up * p0);
                    }
                }
                double pb = pb_off, db = db_off;
                if (m_active) {
                    pb += dec0 * (1.0 - coef) * pb0;
                    DECB[m] += pb0 * (p0 - flux);
                    db += -dec0 * pb0 * ddiag;
                }
                PBP[m] = pb;
                DB[m] += db;
            }
        }
    }
}

/// One sweep in whichever flux form `flux_form` names; `root_w` is read only
/// by LATTICE_FLUX_DETAILED_BALANCE.
inline void lattice_sweep_form(const std::vector<double>& cur, const std::vector<double>& d,
           const std::vector<double>& decay, const std::vector<double>& bounds,
           const std::vector<double>& root_w, std::size_t n, int flux_form,
           std::vector<double>& nxt) {
    if (flux_form == LATTICE_FLUX_DETAILED_BALANCE)
        lattice_sweep_weighted(cur, d, decay, bounds, root_w, n, nxt);
    else
        lattice_sweep(cur, d, decay, bounds, n, flux_form == LATTICE_FLUX_SMOLUCHOWSKI, nxt);
}

/// Propagate `n_steps` explicit steps from `cur`, reporting the population
/// (the plain sum over all voxels) before every `n_out`-th step and once more
/// at the end -- `n_steps / n_out + 1` values in `population_out` -- and
/// return the final density. `weight` is the equilibrium weight field the
/// detailed-balance form reads (empty: w = 1); the other two forms ignore it.
inline std::vector<double> lattice_propagate(
        const std::vector<double>& cur_in, const std::vector<double>& d,
        const std::vector<double>& decay, const std::vector<double>& bounds,
        int ng, int flux_form, int n_steps, int n_out,
        std::vector<double>& population_out,
        const std::vector<double>& weight = std::vector<double>()) {
    const std::vector<double> root_w = flux_form == LATTICE_FLUX_DETAILED_BALANCE
            ? lattice_root_weight(weight, bounds) : std::vector<double>();
    const std::size_t n = static_cast<std::size_t>(ng);
    std::vector<double> cur = cur_in;
    std::vector<double> nxt(cur.size(), 0.0);
    if (n_out < 1) n_out = 1;
    const int n_reports = n_steps / n_out + 1;
    population_out.assign(static_cast<std::size_t>(n_reports), 0.0);

    int i_out = 0;
    for (int step = 0; step < n_steps; ++step) {
        if (step % n_out == 0 && i_out < n_reports) {
            population_out[static_cast<std::size_t>(i_out++)] = lattice_population(cur);
        }
        lattice_sweep_form(cur, d, decay, bounds, root_w, n, flux_form, nxt);
        // Swap **every** step. Swapping only on odd steps while always
        // computing nxt <- f(cur) recomputes the previous step from a stale
        // buffer and throws away half the evolution.
        cur.swap(nxt);
    }
    while (i_out < n_reports) {
        population_out[static_cast<std::size_t>(i_out++)] = lattice_population(cur);
    }
    return cur;
}


inline void lattice_propagate_adjoint(
        const std::vector<double>& cur_in, const std::vector<double>& d,
        const std::vector<double>& decay, const std::vector<double>& bounds,
        int ng, int flux_form, int n_steps, int n_out,
        const std::vector<double>& dL_dF,
        std::vector<double>& dL_dd, std::vector<double>& dL_ddecay,
        std::vector<double>& dL_dcur,
        const std::vector<double>& weight = std::vector<double>()) {
    const bool smoluchowski = (flux_form == LATTICE_FLUX_SMOLUCHOWSKI);
    const bool weighted = (flux_form == LATTICE_FLUX_DETAILED_BALANCE);
    const std::vector<double> root_w = weighted ? lattice_root_weight(weight, bounds) : std::vector<double>();
    const std::size_t n = static_cast<std::size_t>(ng);
    const std::size_t nv = cur_in.size();
    if (n_out < 1) n_out = 1;
    if (n_steps < 0) n_steps = 0;
    const int n_reports = n_steps / n_out + 1;
    if (static_cast<int>(dL_dF.size()) != n_reports)
        throw std::invalid_argument("lattice_propagate_adjoint: dL_dF must have n_steps / n_out + 1 entries");

    dL_dd.assign(nv, 0.0);
    dL_ddecay.assign(nv, 0.0);
    dL_dcur.assign(nv, 0.0);

    // Which report index each step's *pre-sweep* state feeds (or -1); the
    // reports lattice_propagate() emits after the loop all read the final state.
    auto report_of = [&](int step) { return (step % n_out == 0 && step / n_out < n_reports) ? step / n_out : -1; };
    int first_final_report = n_reports;  // reports [first_final_report, n_reports) read p_{n_steps}
    for (int k = 0; k < n_reports; ++k)
        if (k * n_out >= n_steps) { first_final_report = k; break; }

    // Forward with checkpoints every K steps.
    const int K = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(std::max(n_steps, 1))))));
    const int n_ck = n_steps / K + 1;
    std::vector<std::vector<double>> checkpoint(static_cast<std::size_t>(n_ck));
    {
        std::vector<double> cur = cur_in, nxt(nv, 0.0);
        for (int step = 0; step < n_steps; ++step) {
            if (step % K == 0) checkpoint[static_cast<std::size_t>(step / K)] = cur;
            lattice_sweep_form(cur, d, decay, bounds, root_w, n, flux_form, nxt);
            cur.swap(nxt);
        }
        if (n_steps % K == 0) checkpoint[static_cast<std::size_t>(n_steps / K)] = cur;
    }

    // Seed: every report of the final state.
    std::vector<double> pbar(nv, 0.0), pbar_prev(nv, 0.0);
    {
        double seed = 0.0;
        for (int k = first_final_report; k < n_reports; ++k) seed += dL_dF[static_cast<std::size_t>(k)];
        if (seed != 0.0) std::fill(pbar.begin(), pbar.end(), seed);
    }

    // Backward, segment by segment: re-run the forward within the segment to
    // recover p_s for every step in it, then sweep the adjoint back through.
    std::vector<std::vector<double>> seg;
    std::vector<double> nxt(nv, 0.0);
    for (int seg_start = ((n_steps - 1) / K) * K; seg_start >= 0 && n_steps > 0; seg_start -= K) {
        const int seg_end = std::min(seg_start + K, n_steps);  // steps [seg_start, seg_end)
        const int len = seg_end - seg_start;
        seg.resize(static_cast<std::size_t>(len));
        seg[0] = checkpoint[static_cast<std::size_t>(seg_start / K)];
        for (int i = 1; i < len; ++i) {
            seg[static_cast<std::size_t>(i)].assign(nv, 0.0);
            lattice_sweep_form(seg[static_cast<std::size_t>(i - 1)], d, decay, bounds, root_w, n, flux_form,
                               seg[static_cast<std::size_t>(i)]);
        }
        for (int step = seg_end - 1; step >= seg_start; --step) {
            // pbar is the adjoint of p_{step+1}; produce the adjoint of p_step.
            if (weighted)
                lattice_adjoint_sweep_weighted(seg[static_cast<std::size_t>(step - seg_start)], d, decay, bounds,
                                               root_w, pbar, n, pbar_prev, dL_dd, dL_ddecay);
            else if (smoluchowski)
                lattice_adjoint_sweep<true>(seg[static_cast<std::size_t>(step - seg_start)], d, decay, bounds, pbar, n,
                                    pbar_prev, dL_dd, dL_ddecay);
            else
                lattice_adjoint_sweep<false>(seg[static_cast<std::size_t>(step - seg_start)], d, decay, bounds, pbar, n,
                                     pbar_prev, dL_dd, dL_ddecay);
            pbar.swap(pbar_prev);
            const int r = report_of(step);
            if (r >= 0 && r < first_final_report) {
                const double f = dL_dF[static_cast<std::size_t>(r)];
                if (f != 0.0) for (double& v : pbar) v += f;
            }
        }
    }
    dL_dcur = pbar;
}


}  // namespace TTTRLIB_LATTICE_NAMESPACE

#endif  // TTTRLIB_LATTICEDIFFUSION_H
