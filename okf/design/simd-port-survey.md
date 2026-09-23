# Porting from `junk/Simd` — survey (2026-08-28)

**Status: ported 2026-08-28.** P1 collapsed on the check-first rule: Mat.h
already implements the blocking concepts, and `NeuralNet.cpp` already routed
training through them — the port extracted that policy into `MlpGemm.h`,
pinned cross-policy parity, and measured (2.5–5× over PortableGemm at batch
≥ 32; 2.4–2.8× behind Eigen, recorded as the known gap). P2 landed as
`MlpQuant.h` (int8, ~2× double path, ≤ 2% absmax error). P3 as
`RankFilters.h`, P5 as `IntegralImage.h` / `ResizeImage.h` / `FastGaussian.h`,
P4 as `DriftEstimator.h`. Tests in `test/cpp/test_{mlp_gemm,mlp_quant,rank_filters,image_kernels,drift_estimator}.cpp`
(also in the header-only CI job), benchmarks in
`benchmarks/bench_{mlp_gemm,image_kernels}.cpp`. Numbers in
modules/math/README.md and CHANGELOG. Python bindings landed the same day (`ImageOps.h`/`ImageOps.i`, NumPy in /
out, gallery example + smoke test), and the survey's originally-flagged
Sobel/Laplace, WarpAffine, bicubic and histogram/moments are in too
(`Gradients.h`, `WarpAffine.h`, `ResizeImage.h` bicubic, `ImageStat.h`) --
they had fallen between the survey's ported and not-worth lists. `junk/Simd` can now be deleted (kept until the user
re-clone decision; findings all live here).

**Rule (user):** never link the library;
re-express the *technique* in-tree and credit the source. The clone is MIT,
git-stripped, re-clone via `junk/clone.sh`; per the `junk/` contract it goes
once the findings land here.

## What the clone is

ermig1979/Simd, 1170 files. Per operation: a plain-C++ **base** reference
(`SimdBase*.cpp`) plus Sse/Avx2/Avx512/Amx/Neon specialisations. Two families:
image primitives (~90 base ops) and Synet NN inference (a conv/inference zoo —
mostly irrelevant to tttrlib, whose NN stack is MLP-only, double, see below).

In-tree intrinsics precedents a port must reuse (no new patterns):
`simd::` namespace wrap in `modules/simulation/include/SimSimd.h` (AVX2/NEON,
runtime dispatch, scalar fallback), and `TTTRLIB_COMPILE_AVX` +
`TTTRLIB_TARGET_AVX` per-function target attributes in
`modules/imaging/superres/src/CLSMSuperRes.cpp:1010`.

## P1 — blocked/packed GEMM for the NN stack

**Where tttrlib is weaker.** `PortableGemm` (`modules/math/include/MlpCore.h:425`)
is a naive k-outer-product loop under a pragma. The hot ops are `nt` (forward,
MlpCore.h:579) and `tn`/`nn` (backward, :731/:753). Recorded gap: `GradVec` is
13–16 % behind Eigen at two of three sizes (modules/math README) — nobody has
attacked the blocking since.

**What Simd does better** (`SimdGemm.h` + `SimdAvx2Gemm32f.cpp`), four ideas:

1. **Cache-derived macro blocking.** `macroK` from L1, `macroM` from L2,
   `macroN` from L3 (`SimdGemm.h:64-66`), all computed once from the problem
   size, not hard-coded.
2. **Panel packing.** B is re-packed per thread into `microN`-wide,
   K-contiguous panels (`GemmPackB`), which turns the `nt` inner product into a
   streaming `nn`-shaped loop; A is packed into 4x8/6x4 cells. Simd even gates
   PackA off for small K*M (`SimdAvx2Gemm32f.cpp:613`) — packing has a cost.
3. **Register-blocked micro-kernels.** 6x16 / 4x24 / 4x8 accumulator tiles with
   FMA and tail masks, so each loaded element is reused 16–24 times.
4. **Beta fused once** per macro block (`GemmScaleC`), not per k-iteration;
   parallel over N with per-thread packed buffers.

**Port shape.** A `BlockedGemm` as a second `Gemm` template policy in MlpCore
(the policy seam already exists — no API change). Double precision first;
micro-kernel behind the `TTTRLIB_TARGET_AVX` attribute pattern with scalar
fallback.

**Mandatory (AGENTS.md port rule):** benchmark vs the current `PortableGemm`
*and* vs Eigen, `CLOCK_THREAD_CPUTIME_ID`, replicated over seeds, numbers into
the module README. MLP shapes are skinny (batch M ≤ ~10³, K/N ≤ few hundred):
the expected win is the PackB streaming, deep blocking matters less — the
benchmark decides what stays. MlpCore's sklearn round-trip (1e-10) test must
stay green; GEMM reassociation may move last bits, document the max relative
deviation vs `PortableGemm` instead of demanding bit-parity.

## P2 — int8 quantized inference path

`SynetQuantizedInnerProductGemmV0` (base 192 lines, AVX2 233): per-tensor
symmetric int8 weights and activations, int32 accumulation, output requantised
by scale/zero-point. For an MLP this is inner-product-only — Simd's conv
quantisation machinery (the hard 90 % of that subsystem) is not needed.
~150–200 lines std-only plus calibrate/quantise entries. Value: ~2–4×
inference and 4× weight memory for deployed models (burst filter, HMM
surrogate). Parity test = max abs error vs the double path on trained
fixtures, not bit-equality.

## P3 — rank filters 3x3/5x5: median, min, max, midpoint

Technique to lift: **partial sorting networks** (`SortU8`, `PartialSort5`,
`PartialSort9` in `SimdBaseMedianFilter.cpp`) — branch-free min/max chains per
pixel, not window sorts; edges by clamped row pointers. tttrlib has zero 2D
rank filters (1D median only, in BlindIRF). Consumers: denoise before
localisation/superres; chisurf `core/roi`. Caveat: Simd's are u8-only; a port
re-expresses the network in u16 (photon-count images) or uses a 256-bin
sliding histogram for larger radii.

## P4 — ShiftDetector concept (frame drift estimation)

Image pyramid + per-level neighbourhood shift search + 3x3 parabolic subpixel
refinement + stability/correlation scores (`SimdBaseShiftDetector.cpp`, 489
lines). tttrlib has per-channel APR registration but no frame-to-frame drift
estimator: drift is an *input* today (`CLSMSuperRes.cpp:170`). Needs a consumer
decision (CLSM stack API) before porting — do not port speculatively.

## P5 — resizers, integral images, fast Gaussian

- Resizers (area/bilinear/bicubic/nearest) precompute index/alpha tables once
  (`EstimateIndexAlpha`, `SimdBaseResizerBilinear.cpp:41`) — tttrlib's only
  upscale is Fourier zero-pad, a different contract (spectral fidelity, slow).
- Integral images: 87 base lines; consumers would be local sums / mip ladders
  (the `.pto` volume mip ladder is chisurf-side but would consume this).
- Simd's Gaussian is a 3-box approximation, O(1)/px — a *fast* denoising
  kernel, distinct from superres's private scipy-exact separable kernel, which
  must stay for parity contracts. Two names, two contracts, both documented.

## Not worth porting

Conv machinery (Winograd, merged/depthwise, 16b NCHW/NHWC, AMX/bf16) — no conv
layers in the tttrlib NN; revisit only if conv lands. Colour conversions,
font/drawing, HOG/LBP, Base64/CRC, PNG/JPEG codecs (`io_image` covers TIFF;
no consumer). RecursiveBilateralFilter — attractive edge-preserving denoise,
no consumer today.

## Rules any port here must follow

- MIT: keep the upstream copyright notice in the ported header's docstring.
  Comments/docstrings follow the house two-register rule.
- Benchmark rule as in P1; no public API breaks; std-only core; intrinsics
  only behind the two established patterns with runtime dispatch and scalar
  fallback.
- The clone is disposable: when a P-item lands, delete `junk/Simd` (re-clone
  via `junk/clone.sh` if ever re-read) — the finding must live here by then.
