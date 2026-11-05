---
title: PhotonMLE — photon-by-photon micro-time ML of burst lifetimes and anisotropy
status: in-progress
date: 2026-11-05
---

# PhotonMLE

Owner's decision (2026-11-05, verbatim): "needs benchmark. i think photon by photon without
hist, and bin is best, time res preserved, compute saved (can lookup) pimp up burstml with
microtimes. need photon by photon micrtime score. 1. is good maybe coarsening is needed for
cases the photons have too high res. make it dependent of irf." and, later, "photonmle with
aniso lifetime".

Today a burst's lifetime is DecayFit23 run per burst: two BFGS stages over a 2 x ~250-bin
histogram, the IRF convolved over every bin at every evaluation, for a burst with a median of
~70 photons. On DNA13bp_3 that is 10.7 s of the pipeline. PhotonMLE scores the photons
themselves and looks the convolution up in a table built once per detector.

## Where it lives

- `modules/spectroscopy/burst/{include/PhotonMLE.h,src/PhotonMLE.cpp}` — beside `BurstML`. It
  is the micro-time observation model of a burst: `BurstML` is the macro-time (diffusion,
  kinetics) model. The two combine by adding per-photon log-probabilities: `PhotonMLE`
  exposes `photon_log_prob(params, photons)` for that, and `BurstML`'s per-photon colour
  probability can take a micro-time factor later without either class owning the other.
- `burst` does not depend on `decay`, and PhotonMLE keeps that: its convolution is its own
  closed form (below), not `fconv_per_cs`.
- `BurstPipeline::photon_mle(result, [tttr,] config)` runs it on a fused-pipeline result:
  the bursts' photon index ranges (`first_photon`, `last_photon` columns) and the TTTR's
  micro-time and routing-channel columns in place, or the CSR decay rows. No copy of the
  photons is made; results are written back as `pmle|<detector>|<column>` columns.

## The model (per detector, one burst)

Micro-time grid: the IRF's bins, width `dt` (= `binning` raw TAC channels), `n` bins per
polarisation, period `T` (`n * dt <= T`). VV/VH as fit23 (`DecayFit23::modelf`):

    F_VV(t) = IRF_VV (x) [ e^{-k1 t} + r0 (2 - 3 l1) e^{-k2 t} ]
    F_VH(t) = IRF_VH (x) [ e^{-k1 t} - r0 (1 - 3 l2) e^{-k2 t} ] / G
    k1 = 1/tau,  k2 = 1/tau + 1/rho

The convolution is the exact one of a piecewise-constant IRF (each bin a uniform density)
with the periodically summed exponential, `E(u) = e^{-ku} (u >= 0), q e^{-ku} (u < 0)`,
`q = e^{-kT}`, all divided by `1 - q`. Every quantity below is closed form, evaluated by
O(n) recursions per rate (`S1(n) = r S1(n-1) + I_{n-1}`, `r = e^{-k dt}`, and a reverse
sum for the wrap-around) that never divide by `r`.

The probability of photon i, in polarisation p, with micro-time interval `[a_i, b_i)` (its
TAC channel, or its CSR bin), inside the window `W_p` of that polarisation:

    P_i = (1 - gamma - sigma) F_p(a_i, b_i) / Z_F + gamma B_p(a_i, b_i) / Z_B + sigma S_p(a_i, b_i) / Z_S

`F_p(a, b)` is the mass of `F_p` over the interval, `Z_F` its mass over both windows (the
normalisation is over the window, not over the TAC range: a photon outside the window is not
a datum, and the model's tail past the window is not pushed into it). `B` is the measured
background pattern (with the decay's flat floor, as ChiSurf builds it), `S` the IRF as the
scatter shape; both piecewise constant per bin. A detector with one channel (`mirror`) has
unlabelled photons: `P_i` sums both polarisations.

`logL = sum_i w_i log P_i` (`w_i` = 1 per photon, the count of a CSR bin). No histogram of the
burst's photons is built, and a photon's own TAC channel is what is scored.

Parameters: `tau`, `gamma` free; `rho` in one of three modes: `fit` (free, scored by the
photons' polarisation — the default), `perrin` (fit23's rule: `rho = tau / (r0/r - 1)` from
the background-corrected steady-state anisotropy, floor 1e-4 ns — for per-burst parity with
fit23), `fixed`. `r0` fixed by default, freeable; `sigma` (scatter) fixed at 0 by default,
freeable; `G`, `l1`, `l2` from the setup.

## The lookup table (once per detector)

For each polarisation and each node of a grid uniform in `u = log k`, the table holds, at
every table edge `t_e`: the window CDF `C(t_e; k)` (cumulative mass from the window start),
the density `f(t_e; k)`, and their `d/du`. One table covers tau and rho, since the model
needs only the two rates `k1`, `k2`.

- **k interpolation**: cubic Hermite in `u` from the node values and `d/du` — `C^1`, with
  the first and second `u`-derivatives (gradient and Hessian) taken of the same cubic.
- **time interpolation**: a photon's interval mass is `C^(b) - C^(a)`, with `C^` the cubic
  Hermite interpolant of the CDF in time from `C` and `f` at the table edges. Exact (up to the
  k interpolation) when the photon's interval is a table cell, i.e. when nothing is coarsened
  and the photons are on the IRF grid; mass-conserving per cell otherwise.
- **grid spacing**: `du` (default 0.1) bounds the Hermite error by `du^4/384` relative
  (2.6e-7), far below the statistical error of a 20-600-photon burst; set by benchmark.
- **range**: `k` from `1/tau_max` to `1/tau_min + 1/rho_min` (fit23's rho floor included).

### Time-axis coarsening that depends on the IRF

Table step `h = m * dt` with

    m = max(1, floor(FWHM_IRF / (N * dt)))

`FWHM_IRF` from the IRF itself (VV + VH, half-maximum crossings, linear between bins), `N`
configurable (`coarsen_n`, default set by benchmark). The photons are still evaluated at their
own micro-times: within a coarse cell the time Hermite places them. With RVZMT150
(`dt` = 0.08 ns, FWHM ~ 0.5 ns) `m = 1`: nothing is coarsened. With 1-4 ps TAC bins and tens
of thousands of channels the table shrinks by `m` (and so does its build time and memory),
while the time resolution of the photons is kept. `coarsen` = an explicit `m` overrides.

## The exact final step

After the table-driven fit converges, Newton steps with the exact model (closed-form masses
and their exact `u`-derivatives, the Hessian from the table) at the solution until the exact
gradient vanishes; `logL` is reported from the exact model. So results match the exact
likelihood, not the table's.

## Optimiser

- start: tau from the background-corrected mean arrival time inverted through the table's
  window first moment (the IRF and window are accounted for exactly), rho and r from the
  corrected steady-state anisotropy, gamma from background rate x burst duration / photons
  when the rate is known, else the configured start;
- one stage: Levenberg–Marquardt-damped Newton on (log tau, log rho, gamma, r0, sigma), analytic
  gradient and Hessian, box bounds by projection with an active set;
- bursts below `min_photons` are not fitted: NaN and reason 1, as the fused pipeline writes
  them; the other rejection reasons are the pipeline's (`reject_reason`).

## Batch

One call fits every burst of a detector, OpenMP over bursts, no Python inside the loop.
Per-burst work is independent and deterministic: the result does not depend on the thread
count or the burst order. Results are columns owned by `PhotonMLEResult`, exposed as
zero-copy views: `tau, rho, gamma, r0, sigma, r, r_scatter, r_experimental, log_likelihood,
two_istar, n_vv, n_vh, iterations, reason`.

`two_istar` is fit23's 2I* (`-sum_c n_c log(M_c / n_c) / n_channels` on the IRF grid), from
the photons grouped per bin at the end — a report, not the objective.

## Benchmark plan

DNA13bp_3 (all bursts, G|G and R|R are fit23; R|G is a tail fit), Rh110 and Alexa647
references (whole-file fit and 70-photon pseudo-bursts against it), simulated bursts with
known tau/rho/gamma (20-600 photons, 0-30 % background) and a 2 ps synthetic detector for the
coarsening. Wall time, ns/photon, iterations, peak memory, crossings; bias and spread of tau,
rho, r, gamma; per-burst agreement with fit23 (median, p99 |dtau|); table logL vs exact.
Results: tttrlib `PERF.md`, section "PhotonMLE".
