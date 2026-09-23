# `spectroscopy/decay` — Fluorescence Decay Analysis and Fitting

Iterative reconvolution fitting algorithms and maximum likelihood estimation for fluorescence lifetime decay curves.

## Contents

**The fit models and the machinery around them.**

- **`DecayFitModel.h` / `DecayFitModel.cpp`** — the model interface and the factory
  (`make_decay_fit`), plus `DecayFitModelRegistration.h`, which is why a model
  survives being linked out of a static archive.
- **`DecayFitModelFit2x.cpp`** — the Fit2x family (`fit23`, `fit24`, `fit25`,
  `fit26`) and their registry entries.
- **`DecayFitModelNExp.cpp`** — the n-exponential model and its registry entry.
- **`DecayFitPlugin.cpp`** — a model a plugin contributed, wrapped so the
  library's own optimiser fits it.
- **`DecayFit.h` / `DecayFit.cpp`, and `DecayFit23.h` / `DecayFit23.cpp`,
  `DecayFit24.h` / `DecayFit24.cpp`, `DecayFit25.h` / `DecayFit25.cpp`,
  `DecayFit26.h` / `DecayFit26.cpp`** — the kernels:
  the objective, the profiled amplitudes and the analytic pieces of each model.
- **`DecayFitProblem.h` / `DecayFitProblem.cpp`** — a fit as data: parameters, links, bounds,
  results; `DecayFitSetup.cpp` derives the flat parameter layout from the
  registry entry (the flattening rule).
- **`DecayFitContext.h`** — what a kernel is given: the data, the IRF, the
  fitted range and the objective.
- **`DecayFitPrior.h` / `DecayFitPrior.cpp`** — the prior kinds (uniform, normal, truncated
  normal, half-normal, log-normal, exponential, gamma, beta, product, a Python
  callable, and any a plugin adds) and their registry entries.
- **`DecayFitDescriptors.h` / `DecayFitDescriptors.cpp`** — everything this module registers:
  `fit`, `fit_setup`, `objective`, `prior` and its pipeline operations.
- **`DecayStatistics.h` / `DecayStatistics.cpp`** — the objectives: Poisson MLE (2I*), P+2S,
  Neyman and Gehrels least squares, and the Pearson/Neyman chi-squares.
- **`DecayConvolution.h` / `DecayConvolution.cpp`** — the reconvolution kernels (`fconv` and
  its periodic / per-channel / SIMD variants), pile-up, lamp shift, rescaling.
- **`PeriodicDecayKernel.h`** — the bin-integrated periodic decay kernel: what a
  histogram channel receives from light absorbed uniformly within one channel,
  under excitation that repeats every period. Header-only, std-only; imp.bff
  carries a verbatim copy.
- **`DecayFitDFA.h` / `DecayFitDFA.cpp`** — the anisotropy (VV/VH) forms of those kernels.
- **`DecayFitNExp.h` / `DecayFitNExp.cpp`** — the standalone n-exponential fitter.
- **`DecayPatternFit.h` / `DecayPatternFit.cpp`** — the linear unmixing of a decay into
  measured patterns (Poisson MLE or NNLS).
- **`BlindIRF.h` / `BlindIRF.cpp`** — the IRF recovered from a decay alone.

## Examples

- `examples/fluorescence_decay/plot_blind_irf_estimation.py` (+ `.ipynb`): BIRFI blind IRF estimation on a simulated multi-channel decay, then the recovered IRF in a `FitNExp` reconvolution fit.

## Dependencies

- Depends on `core`, `util`.

## Writing a SIMD kernel for a recursion

`fconv_per` and `fconv_per_cs` are recursions: `fitcurr = fitcurr * e + l2[i]`,
one step depending on the last. The obvious way to vectorise that is to put
several lifetimes in one register and run the recursion on all of them — which
is what the first NEON and AVX kernels did, and they were **slower than plain
scalar code**. One register is one dependency chain; an FMA takes several cycles
to retire; and with nothing else in flight the machine waits. Blocked scalar
code with eight independent chains beat two-lane SIMD by 2.6× and four-lane AVX
by more.

What the kernels do now, and what to preserve if you touch them:

* **Several registers, not one.** `R` registers is `R` independent chains and
  `2R` (NEON) or `4R` (AVX) lifetimes in flight. `R` is a template parameter
  chosen per call from the species count — a two-lifetime spectrum in an
  eight-register block pays for the empty lanes.
* **One reduction per channel per block.** The old kernels did a cross-lane
  reduction *and* a read-modify-write of `fit[]` per species-group per channel:
  17 passes over the array for 33 lifetimes. The contributions are now summed in
  a vector accumulator and reduced once.
* **The cap is the register file.** NEON stops at R=8 (32 registers, and the
  main loop keeps 3R live); AVX stops at R=4 (16 registers).

Measured at 1563 channels: 3.4–6.4× over the kernels they replace, across all
three of `fconv`, `fconv_per` and `fconv_per_cs`, and — the point of the
exercise — faster than the blocked scalar kernel at every species count, which
they had not been. Keep `fconv_per_cs_ad<double>` as the yardstick: a
hand-written SIMD kernel that does not beat plain blocked scalar code has
something wrong in it, and that is how this was found. `benchmarks/bench_convolution_kernels.cpp`
reproduces it, and running it with `TTTRLIB_USE_NEON=0` shows the fallback.

The **scalar** kernels are deliberately left unblocked. They are the
obviously-correct body the SIMD ones are checked against, and that is worth more
than their speed; a caller who wants blocked scalar has `fconv_per_cs_ad<double>`.

## Derivatives: `fconv_per_cs_jacobian`

The `_ad` kernels here are templates on the scalar type so they can be
instantiated with `tttrlib::Dual<GradVec<N>>` and produce derivatives from the
same recursion that produces the curve. `fconv_per_cs_jacobian` is that
instantiation reaching the language bindings: it fills the model *and* a
`(n_points, 2*numexp + 1)` Jacobian — one column per lifetime-spectrum entry in
`x`'s own interleaved order, then `d fit / d time_shift`.

Two design points worth keeping:

* **Blocked, not dispatched on the parameter count.** `GradVec<N>` is sized at
  compile time and a spectrum may have any number of lifetimes, so rather than a
  `switch` over instantiations (what `DecayFitNExp` does, where the count is 1–6)
  this uses one width, `FCONV_JAC_BLOCK = 8`, and fills the columns
  `ceil(P/8)` passes at a time. Any parameter count, one instantiation.
* **Every entry of `x` gets a column.** A subset would be policy, and a caller
  fitting only amplitudes can take the columns it wants.

Measured against the `2*P` model evaluations a central-difference Jacobian
costs, at 1563 channels:

| lifetimes | parameters | Jacobian | central differences | |
|---|---|---|---|---|
| 2 | 5 | 0.107 ms | 0.093 ms | **0.9×** |
| 5 | 11 | 0.196 ms | 0.447 ms | 2.3× |
| 16 | 33 | 0.922 ms | 3.290 ms | 3.6× |
| 33 | 67 | 3.967 ms | 13.568 ms | 3.4× |

Below about eight parameters it is a wash or slightly slower — one pass carries
eight lanes whether or not they are used — and the reason to call it there is
that the derivatives are exact, with no step size to choose. Above that it is
three-ish times faster *and* exact.

Getting a `Dual` through the shift needed two fixes that are worth knowing
about, because both had been latent since the templates were written:
`shift_lamp_ad` took a `double*` output, so a dual shift could never have been
stored, and it called `floor` on the dual (there is no such overload). Its
output is now `T*` and the integer part comes from `tttrlib::ad_value(ts)` — the
shift's integer part is piecewise constant, so the fraction carries the whole
derivative. `fconv_per_cs_ad` gained a second, defaulted template parameter for
the response's sample type, so the shifted IRF can carry `d/d(shift)` into the
convolution; every existing call site is unchanged and the `double`
instantiation compiles to the same code.

## `stop` bounds the output, not the precomputed response

`fconv_per`'s recursion runs to `stop1 = min(period_n + lamp_start, n_points)`,
which is bounded by the point count and **not** by the caller's `stop`. The
`dt/2 * lamp` array it reads along the way must therefore be sized by
`n_points`. All three kernels sized it by `stop` until 2026-09-09, so a caller
passing `stop = n_points - 1` read one element past the end — and a caller
passing a stop well short of the point count read further.

The symptom is worth remembering because it does not look like an overrun. The
function appeared to hold *state*: allocating a fresh response and a fresh
zeroed output on every call, the result still grew by about one species' worth
per call. What grew was the heap — the previous call's freed arrays were what
lay past the end of the buffer. Anything that reads uninitialised or recycled
memory will look like history dependence before it looks like a bounds bug, and
the first instinct (the library is caching something) sends you looking in the
wrong place.

## Whether a convolution clears its output buffer

Every convolution here **overwrites** `fit`; none accumulates. That is worth
stating because until 2026-09-09 `fconv_per()` did each of them depending on
which kernel the runtime dispatch chose: the scalar kernel never cleared the
buffer and both SIMD kernels did. Since `kSimdMinNumexp` is 2, that meant a **one-lifetime model accumulated
and a two-lifetime model did not, on the same machine, with nothing set** — a
fit loop reusing its buffer saw a single-exponential curve grow without bound.
The scalar kernel now clears. The two paths were, and remain, bit-identical on a
cleared buffer; only the clearing differed.

If you add a kernel to this file, this is the property to check first, and to
check it you have to drive both dispatch paths — `TTTRLIB_USE_NEON=0` /
`TTTRLIB_USE_AVX=0` in a subprocess, as
`test_simd_convolution_correctness.py::TestTheScalarAndSimdKernelsActuallyAgree`
does. The rest of that file could not have caught this: it compares `fconv_per`
against `fconv_per_simd`, which are the same function since the alias was
deprecated, and its classes are `skipUnless(get_avx_enabled())`, so on AArch64
they all skip.

## The model floor in the Poisson likelihoods

`Wcm` and `wcm_p2s` (`DecayStatistics`) are the objectives `DecayFit23/24/25/26`
minimise. Both treat a model bin at or below `kModelFloor = 1e-12` specially,
and the way they do it changed — the reason is worth knowing before touching it.

They used to **skip** such a bin, under a comment reading "this is only for
stability reasons". It was not stability. The term a near-zero bin contributes
to the minimised objective is `-C·log(m)`, which is large and *positive*: at
`C = 30` and `m = 1e-12`, about `+829`. Skipping it is a discontinuous
improvement of exactly that size, awarded for pushing the bin one step further
down — and below the floor the objective is perfectly flat, so nothing pulls it
back. Measured: the objective falls **828.9** across the threshold and is
identical for every negative model value.

`log` is now continued below the floor by its tangent there,
`log m₀ + (m − m₀)/m₀`, which agrees in value *and* slope. The objective is C1
across the floor, finite for every finite model value including negative ones,
and strictly worse the further below it goes. `wcm_p2s` gets the same treatment
at C0 — its series' true slope at the floor is not `1/m₀`, and matching it would
mean differentiating the sum for a region no converged fit should visit.

**This moves two reference fits — and the first version of this note said it
moved none.** Above the floor the arithmetic is bit-for-bit what it was, and
that part is solid. The claim that went further rested on a sweep of 143,360
model bins across the clamped `DecayFit23` box that never produced a bin below
`1.86e-07` — but **that sweep used a flat non-zero background**, which is not
what the reference data is. It proved the floor unreachable for the inputs it
happened to choose.

Where the model does reach the floor:

| fit | before | after | why |
|---|--:|--:|---|
| `fit23` 2I* | 23.802337 | 23.791124 | zero background, 58 photons |
| `fit23` tau | 0.74219 | 0.721353 | |
| `fit25` 2I* | 4.738831 | 3.887975 | p2s path — `wcm_p2s` dropped the **pair** if *either* channel underflowed |
| `fit24`, `fit26` | unchanged | unchanged | background 0.2 |

`fit25` is the clean attribution: the only other change on that path removed an
always-zero addend, so all 0.85 of the movement is the likelihood correction.
The old values answered a likelihood that discarded occupied bins; they are not
the more correct ones. Generalisable lesson: **an inertness sweep proves nothing
outside the inputs it sweeps**, and a background of zero is exactly the corner a
"representative" parameter box omits.

Two things not to undo:

* **The multiply stays in `Wcm`'s loop body**, not inside `log_m_ext`. Inside,
  the compiler stops contracting it into the accumulate and every ordinary
  evaluation shifts by an ulp — in functions pinned by cross-language reference
  tests. `test/cpp/test_decay_likelihood.cpp` checks this bitwise.
* **`twoIstar`/`twoIstar_p2s` are deliberately left alone.** They guard on the
  *counts* rather than the model, so unlike `Wcm` they genuinely can return NaN
  for a non-positive model — but they are computed after the fit for reporting
  and are never minimised, and changing them would move numbers the reference
  tests pin for no benefit to any optimiser.

## Bounds in `DecayFit23`: one mechanism, and why the clamps stay

A bound and a prior are the same object: minimising `−log L + p(x)` is MAP
estimation with `p = −log prior`. `DecayFitContext.h` says so directly — *"A
bound is a uniform prior in this interface"* — and it follows that
`i_lbfgs::set_bounds`, a smooth exterior penalty `k(x−hi)²`, is already a proper
prior (flat-topped, Gaussian-shouldered). It is the one mechanism this fit uses.

It did not used to be. There were three:

* **`set_bounds` for gamma** — but only inside the branch that frees gamma, so
  the pre-fit ran under different rules than the main fit. Now set once,
  unconditionally, with every other bound.
* **A hand-rolled term for tau**, `penalty = (x[0] < kMinTau) ? -x[0] : 0`. This
  was **wrong**: it is negative over the whole band `0 < tau < kMinTau`, so
  crossing below the bound *improved* the objective (measured: 9e-4 better
  stepping from 1.1e-3 to 9e-4), and it was discontinuous at the bound. Removed.
* **Clamps in `sanitise_parameters`** — which stay, see below.

### The floors are numerical guards, not constraints

They cannot be deleted, and the reason is measurable rather than a matter of
taste: without the `tau` floor, `exp(-dt/tau)` overflows for `tau` in roughly
`(-dt/709, 0)`. At `tau = -1e-6` every model bin comes back `inf` and the
objective is `NaN` — and no penalty rescues a `NaN`, because the line search has
to be able to *score* the point it proposes.

`tau` and `rho` therefore go through `soft_floor` (`DecayFit.h`) — **exactly**
the identity at and above the floor, so no ordinary fit moves by an ulp, and
`m₀·exp((v−m₀)/m₀)` below it: C1 at the join, strictly positive for every finite
input, nonzero derivative throughout. The failure direction flips from overflow
to underflow, which the likelihood's own floor continuation absorbs.

**The smooth floor does not make the objective non-flat below `kMinTau`, and it
cannot.** That flatness is physical: at `dt = 0.032`, `exp(-dt/tau)` is already
`1.3e-14` at `tau = 1e-3` and underflows below, so the model is saturated. The
proof that the clamp was never the cause is that `d/dtau` is already exactly 0
at `tau = 1.1e-3` — *above* the floor. `tau_eff` keeps moving under the
transform; the model stops caring. Do not try to tune `kMinTau` to fix this; no
parameter map can manufacture information the likelihood does not contain. The
restoring force there comes from `set_bounds`.

A clamp is separately the wrong thing to enforce a bound *with*, because a
clamped objective is **flat** outside the box: `d/dgamma` is exactly 0 at
gamma = 1.0, 1.2 and 2.0. Nothing points home. `gamma` keeps its hard clamp —
unlike `tau` it has no arithmetic failure outside its range (the model is finite
at gamma = −0.2 and 1.5), so its clamp is purely a modelling constraint and
removing it is a behaviour change worth making on its own.

So the two roles are separated. The floors keep the model evaluable; the soft
bounds shape the fit. That split is also what made the AD conversion possible:
`i_lbfgs` adds the bound penalty *and its gradient* to whatever a registered
analytic-gradient callback returns (`i_lbfgs.h:313-320`), whereas a term added
to the objective by hand — as the old `tau` penalty was — is invisible to that
callback and would have made the analytic gradient wrong by exactly `-1` in the
`tau` component below the bound.

### The general (tau/gamma) branch now uses that gradient

`decay23_gradient` (`DecayFit23.cpp`, anonymous namespace) is a one-pass
forward-mode gradient — `tttrlib::Dual<GradVec<4>>`, the same machinery the
2D-Gaussian localization fit uses — registered via `bfgs_o.set_gradient(...)`
in place of `i_lbfgs`'s central-difference default. It differentiates the same
chain `targetf`/`modelf` runs (`sanitise_parameters`'s soft floor and hard
clamp, the derived-or-fixed `rho`, both `fconv_per_cs` calls, the background
mix, `normM`, `Wcm`) via templated siblings of those functions
(`fconv_per_cs_ad`/`Wcm_ad`/`log_m_ext_ad`/`soft_floor_ad`/`clamp_value_ad`),
not a reimplementation next to them. `Wcm_p2s`'s series expansion is not
templated, so `fit_settings.p2s_twoIstar` fits keep using central differences.

Measured (`benchmarks/bench_decayfit23_ad.py`, `benchmarks/bench_decayfit23_batch_ad.py`):
one `Fit23()` call through Python is **1.27×** faster; `fit_many` on 8000 rows
(`tttrlib::parallel_for`, all cores) is **1.68×** faster wall clock, with fitted
values unchanged. See PRD-010's Phase 6 and `PERF.md`'s "Exact gradient in
DecayFit23's general (tau/gamma) branch" for the full numbers.

### `DecayFit24`: an exact gradient was tried, tested, and declined

`tau1`/`tau2` moved to `soft_floor` (the same arithmetic-overflow guard as
`DecayFit23`'s `tau`) — this part shipped and stays. An exact gradient was
then built the same way as `DecayFit23`'s, for the two-lifetime model (N=5:
tau1/gamma/tau2/A2/offset), reusing `fconv_per_cs_ad`/`Wcm_ad` unchanged (only
`normM_p2s`'s per-half scaling needed new templated code); `A2`, `gamma` and
`offset` stay hard-clamped, deliberately, for the same reason `DecayFit23`'s
`gamma` does. One thing found and preserved rather than fixed while building
it: `correct_input`'s gamma clamp *tests* `x[1] > 0.999 - xm[3]` (coupled to
`A2`) but *assigns* the flat constant `0.999`, not `0.999 - xm[3]` —
reproducing `correct_input` exactly was the job, not correcting a latent bug
in it.

Measured at 8000 rows: wall clock **1.08×–1.15×** faster, but total CPU
across worker threads — the more repeatable metric — **roughly flat to 6%
slower**. Unlike `DecayFit23` this was not a clean win, so the gradient was
**not shipped** — removed from `DecayFit24.cpp` (a note above `modelf` says
what was tried and why), same call already made for `DecayFit26`.
`test/cpp/test_ad_gradient.cpp`'s `decay24` section stays, as the record that
the removed approach was correct, not merely attempted. See PRD-010's Phase 7
and `PERF.md` for the full numbers and the likely cause.

### Also unified

`DecayFit25` and `DecayFit26` carried the same idea and are now on `set_bounds`
too — see the section below.

### The same tidy in `DecayFit25` and `DecayFit26`

Both carried a thread-local `penalty` added to the objective in `targetf`.

`DecayFit25`'s was **dead** — set to zero and never to anything else. Removed.

`DecayFit26`'s was **correct**, unlike fit23's: `-x[0]` below zero and `x[0]-1`
above one, both positive outside the box. So this was a tidy, not a bug fix.
It still had fit23's other two problems — it duplicated a mechanism `i_lbfgs`
already provides, and being added to the objective outside the model it is
invisible to an analytic gradient, which sees only what the registered callback
returns. Replaced by `set_bounds(0, 0.0, 1.0)`; the clamp in `correct_input`
stays as the arithmetic guard.

Verified the same way as fit23: every in-range starting point gives an identical
fraction and 2I*; a start at `f = -0.3` differs in the sixth decimal with the
same 2I*, i.e. the same minimum reached by a marginally different path.

## `DecayFitNExp`: a joint AD refinement pass, additive to the shipped search

`DecayFitNExp.cpp` deliberately never used `bfgs`: amplitudes are profiled by
EM (closed-form given fixed lifetimes, since they enter the model linearly)
and lifetimes are searched one at a time by a Brent search that is explicitly
multistart-aware ("a profiled mixture likelihood need not be unimodal in one
lifetime", `DecayFitNExp.cpp`). Both properties are real and are unchanged —
this adds a joint gradient step *after* the coordinate search converges,
never instead of it.

`refine_lifetimes_ad` (`DecayFitNExp.cpp`, anonymous namespace) profiles
amplitudes with the *same* EM (`evaluate_profile_ws`, plain `double`, re-run
at every trial lifetime vector) and takes the AD gradient
(`tttrlib::Dual<GradVec<N>>`, reusing `fconv_per_cs_ad`) holding those
amplitudes constant. This is exact by the envelope theorem: at the EM
optimum `d(NLL)/d(weight) = 0`, so `d/d(tau)[profiled NLL]` equals the
partial derivative of `NLL(tau, weights)` holding weights fixed — the term
through weights' own dependence on tau vanishes identically, so there is no
need to differentiate through the EM iteration itself. `N` is a runtime
value but `Dual<GradVec<N>>` needs it at compile time, so
`refine_lifetimes_ad_dispatch` switches on it for `N = 1..6` and silently
skips the refinement beyond that (real fits are 1-4 exponentials).

Multistart robustness is deliberately not reimplemented: a prototype
(`benchmarks/bench_fitnexp_bfgs_ad.cpp`, kept for the record) found that a
*cold* joint start with no grid scan can land in a worse local optimum than
Brent's multistart finds, on data where refining Brent's own answer
afterward improves it. So the refinement only ever polishes the coordinate
search's own converged answer. `bfgs`'s Armijo line search only accepts
strictly decreasing steps, so it cannot make that answer worse by
construction, not merely by what was measured.

Measured at scale (batched, realistic per-curve photon counts,
`DecayFitNExp::fit_batch_flat`): **0.5% overhead**, **100%** of tested rows
improved, **0%** regressed. One real regression was found and fixed before
shipping: `N=1` (mono-exponential, the library's single most benchmarked
path — `PERF.md`'s "Single-curve lifetime fit") has no cross-lifetime
correlation for a joint step to recover, so the refinement there was pure
overhead — measured at +35% per call for an unchanged answer, so it is
gated to `N >= 2`. The fixed-lifetime `fit_map` path (`fixed=[1]`,
`ext/python/FitNExpWrapper.py`) never has a free lifetime, so the
refinement's `any_free` guard makes it structurally unreachable there
regardless of `N`. See PRD-010's Phase 10 and `PERF.md` for the full
numbers.
