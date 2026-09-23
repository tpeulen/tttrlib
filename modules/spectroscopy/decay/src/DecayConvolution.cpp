// SPDX-License-Identifier: BSD-3-Clause
#include "DecayConvolution.h"
#include "Registry.h"
#include "Verbose.h"
#include "info.h"
#include "GradVec.h"   /* the Jacobian pass carries GradVec<FCONV_JAC_BLOCK> */
#include <stdexcept>
#include <vector>
#include <algorithm>

/* rescaling -- old version. sum(fit)->sum(decay) */
void rescale(double *fit, double *decay, double *scale, int start, int stop) {
    /* scaling */
    if (*scale == 0.) {
        double sumfit = 0., sumcurve = 0.;
        for (int i = start; i < stop; i++) {
            sumfit += fit[i];
            sumcurve += decay[i];
        }
        if (sumfit != 0.) *scale = sumcurve / sumfit;
    }
    for (int i = start; i < stop; i++)
        fit[i] *= *scale;
}

/* rescaling -- new version. scale = sum(fit*decay/w^2)/sum(fit^2/w^2) */
void rescale_w(double *fit, double *decay, double *w_sq, double *scale, int start, int stop) {
    /* scaling */
    if (*scale == 0.) {
        double sumnom = 0., sumdenom = 0.;
        for (int i = start; i < stop; i++) {
            if (decay[i] != 0.) {
                sumnom += fit[i] * decay[i] / w_sq[i];
                sumdenom += fit[i] * fit[i] / w_sq[i];
            }
        }
        if (sumdenom != 0.) *scale = sumnom / sumdenom;
    }
    for (int i = start; i < stop; i++)
        fit[i] *= *scale;

}

/* rescaling -- new version + background. scale = sum(fit*decay/w^2)/sum(fit^2/w^2) */
void rescale_w_bg(double *fit, double *decay, double *e_sq, double bg, double *scale, int start, int stop) {
    double sumnom = 0., sumdenom = 0.;
    for (int i = start; i < stop; i++) {
        if(decay[i] > 0){
            double iwsq = (e_sq[i]*e_sq[i]+1e-12);
            sumnom += fit[i] * (decay[i] - bg) * iwsq;
            sumdenom += fit[i] * fit[i] * iwsq;
        }
    }
    if (sumdenom != 0.) *scale = sumnom / sumdenom;
    for (int i = start; i < stop; i++)
        fit[i] *= *scale;
if (is_verbose()) {
    std::clog << "RESCALE_W_BG" << std::endl;
    std::clog << "w_sq [start:stop]: "; for(int i=start; i<stop; i++) std::clog << e_sq[i] << " "; std::clog << std::endl;
    std::clog << "decay [start:stop]: "; for(int i=start; i<stop; i++) std::clog << decay[i] << " "; std::clog << std::endl;
    std::clog << "fit [start:stop]: "; for(int i=start; i<stop; i++) std::clog << fit[i] << " "; std::clog << std::endl;
    std::clog << "-- sumnom: " << sumnom << std::endl;
    std::clog << "-- sumdenom: " << sumdenom << std::endl;
    std::clog << "-- final scale: " << *scale << std::endl;
}
}


// fast convolution - scalar reference implementation.
static void fconv_scalar(double *fit, double *x, double *lamp, int numexp, int start, int stop, double dt) {
    // The body lives in the header as `fconv_ad`, so a header-only consumer
    // (imp.bff's TCSPC decay node) runs this very implementation.
    fconv_ad<double>(fit, x, lamp, numexp, start, stop, dt);
}


#if TTTRLIB_COMPILE_AVX
// AVX+FMA kernel for fconv(). Only called after a runtime CPUID check confirms
// the host supports AVX and FMA (see fconv_simd() dispatcher below); the target
// attribute lets it use AVX/FMA even when the TU is built without -mavx.
/// Horizontal sum of a `__m256d`, the standard two-step fold.
TTTRLIB_TARGET_AVX_FMA
static inline double fconv_hsum256(__m256d v) {
    const __m128d low = _mm256_castpd256_pd128(v);
    const __m128d high = _mm256_extractf128_pd(v, 1);
    const __m128d sum = _mm_add_pd(low, high);
    return _mm_cvtsd_f64(_mm_add_sd(sum, _mm_unpackhi_pd(sum, sum)));
}

// Non-periodic convolution, AVX+FMA: `R` registers, 4R species in flight. Same
// rewrite and the same reasons as `fconv_per_avx_block`; the speedup is
// inferred from the NEON measurement, not measured on x86.
template <int R>
TTTRLIB_TARGET_AVX_FMA
static void fconv_avx_block(double *fit, double *x, double *lamp, int numexp,
                            int start, int stop, double dt) {
    constexpr int kSpecies = 4 * R;
    const int start1 = std::max(1, start);
    const int n_ele = ((numexp + kSpecies - 1) / kSpecies) * kSpecies;

    auto *p = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    auto *ex = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    std::fill(p, p + n_ele, 0.0);
    std::fill(ex, ex + n_ele, 0.0);
    for (int i = 0; i < numexp; i++) {
        p[i] = x[2 * i + 0];
        ex[i] = exp(-dt / x[2 * i + 1]);
    }
    auto l2 = (double *) malloc(stop * sizeof(double));
    for (int i = 0; i < stop; i++) l2[i] = dt * 0.5 * lamp[i];

    std::fill(fit, fit + stop, 0.0);
    for (int base = 0; base < numexp; base += kSpecies) {
        __m256d e[R], a[R], fitcurr[R];
        for (int r = 0; r < R; ++r) {
            e[r] = _mm256_load_pd(&ex[base + 4 * r]);
            a[r] = _mm256_load_pd(&p[base + 4 * r]);
            fitcurr[r] = _mm256_setzero_pd();
        }
        {
            const __m256d l0 = _mm256_set1_pd(l2[0]);
            __m256d acc = _mm256_setzero_pd();
            for (int r = 0; r < R; ++r) acc = _mm256_fmadd_pd(l0, a[r], acc);
            fit[0] += fconv_hsum256(acc);
        }
        for (int i = start1; i < stop; ++i) {
            const __m256d lo = _mm256_set1_pd(l2[i - 1]);
            const __m256d hi = _mm256_set1_pd(l2[i]);
            __m256d acc = _mm256_setzero_pd();
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = _mm256_fmadd_pd(_mm256_add_pd(fitcurr[r], lo), e[r], hi);
                acc = _mm256_fmadd_pd(fitcurr[r], a[r], acc);
            }
            fit[i] += fconv_hsum256(acc);
        }
    }
    _mm_free(p); _mm_free(ex); free(l2);
}

/// Block width by species count; capped at 4 registers for the x86 register file.
TTTRLIB_TARGET_AVX_FMA
static void fconv_avx_impl(double *fit, double *x, double *lamp, int numexp,
                           int start, int stop, double dt) {
    if (numexp <= 4)
        fconv_avx_block<1>(fit, x, lamp, numexp, start, stop, dt);
    else if (numexp <= 8)
        fconv_avx_block<2>(fit, x, lamp, numexp, start, stop, dt);
    else
        fconv_avx_block<4>(fit, x, lamp, numexp, start, stop, dt);
}
#endif // TTTRLIB_COMPILE_AVX

#if TTTRLIB_COMPILE_NEON
// NEON kernel for fconv(): processes 2 lifetimes per float64x2_t. The per-
// lifetime recurrence cannot be autovectorized, so this manual 2-wide version
// wins (~1.75x on Apple M1). NEON is baseline on AArch64 - no CPUID needed.
// Non-periodic convolution, NEON: `R` registers, 2R species in flight. Same
// rewrite and the same reasons as `fconv_per_neon_block` -- see that kernel.
template <int R>
static void fconv_neon_block(double *fit, double *x, double *lamp, int numexp,
                             int start, int stop, double dt) {
    constexpr int kSpecies = 2 * R;
    const int start1 = std::max(1, start);
    const int n_ele = ((numexp + kSpecies - 1) / kSpecies) * kSpecies;

    std::vector<double> p(n_ele, 0.0), ex(n_ele, 0.0), l2(stop);
    for (int i = 0; i < numexp; i++) { p[i] = x[2 * i]; ex[i] = exp(-dt / x[2 * i + 1]); }
    for (int i = 0; i < stop; i++) l2[i] = dt * 0.5 * lamp[i];

    std::fill(fit, fit + stop, 0.0);
    for (int base = 0; base < numexp; base += kSpecies) {
        float64x2_t e[R], a[R], fitcurr[R];
        for (int r = 0; r < R; ++r) {
            e[r] = vld1q_f64(&ex[base + 2 * r]);
            a[r] = vld1q_f64(&p[base + 2 * r]);
            fitcurr[r] = vdupq_n_f64(0.0);
        }
        {
            const float64x2_t l0 = vdupq_n_f64(l2[0]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) acc = vfmaq_f64(acc, l0, a[r]);
            fit[0] += vaddvq_f64(acc);
        }
        for (int i = start1; i < stop; ++i) {
            const float64x2_t lo = vdupq_n_f64(l2[i - 1]);
            const float64x2_t hi = vdupq_n_f64(l2[i]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = vfmaq_f64(hi, vaddq_f64(fitcurr[r], lo), e[r]);
                acc = vfmaq_f64(acc, fitcurr[r], a[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
    }
}

/// Block width by species count; see `fconv_per_neon_impl` for the measurements.
static void fconv_neon_impl(double *fit, double *x, double *lamp, int numexp,
                            int start, int stop, double dt) {
    if (numexp <= 4)
        fconv_neon_block<2>(fit, x, lamp, numexp, start, stop, dt);
    else if (numexp <= 8)
        fconv_neon_block<4>(fit, x, lamp, numexp, start, stop, dt);
    else
        fconv_neon_block<8>(fit, x, lamp, numexp, start, stop, dt);
}
#endif // TTTRLIB_COMPILE_NEON

// Below this many lifetimes the SIMD kernels do not pay off: they vectorise
// ACROSS lifetimes and zero-pad to the register width, so a single-exponential
// spectrum does the same work with extra setup. Measured on AArch64/NEON
// (n=1024): numexp=1 is 1.02x for fconv and 0.89x -- i.e. a REGRESSION -- for
// fconv_per, while numexp>=2 is a consistent 1.65-1.85x win. Selecting on CPU
// features alone would therefore make single-exponential fits slower.
static const int kSimdMinNumexp = 2;

static inline bool simd_convolution_available(int numexp) {
    if (numexp < kSimdMinNumexp) return false;
#if TTTRLIB_COMPILE_AVX
    return tttrlib::cpu_features::get_avx_enabled() && tttrlib::cpu_features::get_fma_enabled();
#elif TTTRLIB_COMPILE_NEON
    return tttrlib::cpu_features::get_neon_enabled();
#else
    return false;
#endif
}

// fast convolution - picks the best available kernel automatically (AVX/FMA on
// x86_64, NEON on AArch64, scalar otherwise), based on both the host CPU and
// the problem size. Callers do not need to know which kernels exist.
void fconv(double *fit, double *x, double *lamp, int numexp, int start, int stop, double dt) {
    if (simd_convolution_available(numexp)) {
#if TTTRLIB_COMPILE_AVX
        fconv_avx_impl(fit, x, lamp, numexp, start, stop, dt);
        return;
#elif TTTRLIB_COMPILE_NEON
        fconv_neon_impl(fit, x, lamp, numexp, start, stop, dt);
        return;
#endif
    }
    fconv_scalar(fit, x, lamp, numexp, start, stop, dt);
}

/// Deprecated alias of fconv(): the scalar/SIMD choice is made inside fconv()
/// itself, so this name promises a decision the caller does not have. Kept as
/// a shim for one release (exported, and downstream callers exist); remove
/// after that.
void fconv_simd(double *fit, double *x, double *lamp, int numexp, int start, int stop, double dt) {
    fconv(fit, x, lamp, numexp, start, stop, dt);
}



/* fast convolution, high repetition rate - scalar reference implementation. */
static void fconv_per_scalar(double *fit, double *x, double *lamp, int numexp, int start, int stop,
               int n_points, double period, double dt)
{
    stop = (stop < 0) ? n_points: stop;
    int period_n = (int)ceil(period/dt-0.5);

    int lamp_start = 0;
    while (lamp_start < stop && lamp[lamp_start++] == 0);

    int start1 = std::max(1, start);
    int stop1 = std::min(period_n+lamp_start, n_points);

    if (is_verbose()) {
    std::clog << "FCONV_PER" << std::endl;
    std::clog << "-- numexp:" << numexp << std::endl;
    std::clog << "-- start:" << start << std::endl;
    std::clog << "-- stop:" << stop << std::endl;
    std::clog << "-- n_points:" << n_points << std::endl;
    std::clog << "-- period:" << period << std::endl;
    std::clog << "-- dt:" << dt << std::endl;
}

    // Precompute everything needed for the convolution
    // lamp * dt * 0.5
    // Sized by n_points, not by `stop`. The recursion below runs to `stop1`,
    // which is bounded by n_points and NOT by `stop`, so any caller passing a
    // stop short of the point count -- `stop = n_points - 1` is enough --
    // walked off the end of this buffer and convolved with whatever the heap
    // held there. It read as *state*: allocate a fresh IRF and a fresh output
    // each call and the answer still grew by one species' worth per call,
    // because the freed arrays of the previous call were what lay past the
    // end. Found 2026-09-09 from a caller's reproducer.
    auto l2 = (double *) malloc(n_points * sizeof(double));
    for (int i = 0; i < n_points; i++) l2[i] = dt * 0.5 * lamp[i];

    // `fit` is an out parameter, so clear it: the loop below accumulates one
    // lifetime at a time. Both SIMD kernels have always done this and this one
    // did not, which made fconv_per()'s contract depend on which kernel the
    // dispatch picked -- accumulate at numexp == 1 (below kSimdMinNumexp, so
    // always scalar) and overwrite at numexp >= 2, on the same machine. A fit
    // loop reusing its model buffer got a single-exponential model that grew
    // without bound and a multi-exponential one that did not.
    std::fill(fit, fit + n_points, 0.0);

    /* convolution */
    for (int ne=0; ne<numexp; ne++) {
        double expcurr = exp(-dt/x[2*ne+1]);
        double tail_a = 1./(1.-exp(-period/x[2*ne+1]));
        double fitcurr = 0;
        fit[0] += (fitcurr + l2[0])*x[2*ne];
        for (int i=start1; i<stop1; i++){
            fitcurr=(fitcurr + l2[i - 1])*expcurr + l2[i];
            fit[i] += fitcurr*x[2*ne];
        }
        fitcurr *= exp(-(period_n - stop1 + start)*dt/x[2*ne+1]);
        for (int i=start; i<stop; i++){
            fitcurr *= expcurr;
            fit[i] += fitcurr*x[2*ne]*tail_a;
        }
    }
    free(l2);
}


#if TTTRLIB_COMPILE_AVX
// AVX+FMA kernel for fconv_per(); dispatched only on AVX+FMA capable CPUs.
// Periodic convolution, AVX+FMA: `R` registers advanced together, so 4R species
// are in flight at once. The same rewrite as `fconv_per_neon_block`, for the
// same reasons -- see that kernel's comment for the measurements that motivated
// it. This variant did the arithmetic four species at a time in one register,
// which is one dependency chain against the FMA latency, and reduced it to a
// scalar by *storing the register to memory and adding four doubles back* once
// per channel per species-quad.
//
// The speedup here is inferred from the NEON measurement, not measured: this is
// developed on AArch64 and no x86 machine was available. What is checked on
// both is that the SIMD and scalar kernels agree
// (`test_simd_convolution_correctness.py`, which runs its AVX classes only
// where AVX exists, plus the ungated class that drives both dispatch paths).
template <int R>
TTTRLIB_TARGET_AVX_FMA
static void fconv_per_avx_block(double *fit, double *x, double *lamp, int numexp,
                                int start, int stop, int n_points, double period,
                                double dt) {
    constexpr int kSpecies = 4 * R;
    const int start1 = std::max(1, start);
    stop = (stop < 0) ? n_points : stop;
    const int n_ele = ((numexp + kSpecies - 1) / kSpecies) * kSpecies;

    const int period_n = (int)ceil(period / dt - 0.5);
    int lamp_start = 0;
    while (lamp_start < stop && lamp[lamp_start++] == 0);
    const int stop1 = std::min(period_n + lamp_start, n_points);

    // Sized by n_points, not `stop`: the recursion runs to `stop1`, which is
    // bounded by the point count. See the module README.
    auto l2 = (double *) malloc(n_points * sizeof(double));
    for (int i = 0; i < n_points; i++) l2[i] = dt * 0.5 * lamp[i];

    auto ex = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    auto p = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    auto scale = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    auto tails = (double *) _mm_malloc(n_ele * sizeof(double), 32);
    std::fill(ex, ex + n_ele, 0.0);
    std::fill(p, p + n_ele, 0.0);
    std::fill(scale, scale + n_ele, 0.0);
    std::fill(tails, tails + n_ele, 0.0);
    for (int i = 0; i < numexp; i++) {
        ex[i] = exp(-dt / x[2 * i + 1]);
        p[i] = x[2 * i];
        scale[i] = exp(-(period_n - stop1 + start) * dt / x[2 * i + 1]);
        tails[i] = 1. / (1. - exp(-period / x[2 * i + 1]));
    }
    // Padding lanes carry a zero amplitude and contribute exactly nothing.

    std::fill(fit, fit + n_points, 0.0);
    for (int base = 0; base < numexp; base += kSpecies) {
        __m256d e[R], a[R], s[R], t[R], fitcurr[R];
        for (int r = 0; r < R; ++r) {
            e[r] = _mm256_load_pd(&ex[base + 4 * r]);
            a[r] = _mm256_load_pd(&p[base + 4 * r]);
            s[r] = _mm256_load_pd(&scale[base + 4 * r]);
            t[r] = _mm256_load_pd(&tails[base + 4 * r]);
            fitcurr[r] = _mm256_setzero_pd();
        }
        {
            const __m256d l0 = _mm256_set1_pd(l2[0]);
            __m256d acc = _mm256_setzero_pd();
            for (int r = 0; r < R; ++r) acc = _mm256_fmadd_pd(l0, a[r], acc);
            fit[0] += fconv_hsum256(acc);
        }
        for (int i = start1; i < stop1; ++i) {
            const __m256d lo = _mm256_set1_pd(l2[i - 1]);
            const __m256d hi = _mm256_set1_pd(l2[i]);
            __m256d acc = _mm256_setzero_pd();
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = _mm256_fmadd_pd(_mm256_add_pd(fitcurr[r], lo), e[r], hi);
                acc = _mm256_fmadd_pd(fitcurr[r], a[r], acc);
            }
            fit[i] += fconv_hsum256(acc);
        }
        for (int r = 0; r < R; ++r) fitcurr[r] = _mm256_mul_pd(fitcurr[r], s[r]);
        for (int i = start; i < stop; ++i) {
            __m256d acc = _mm256_setzero_pd();
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = _mm256_mul_pd(fitcurr[r], e[r]);
                acc = _mm256_fmadd_pd(_mm256_mul_pd(fitcurr[r], a[r]), t[r], acc);
            }
            fit[i] += fconv_hsum256(acc);
        }
    }
    free(l2); _mm_free(p); _mm_free(ex); _mm_free(scale); _mm_free(tails);
}

/// Block width by species count. Capped at 4 registers: the main loop keeps
/// `3R` vectors live and x86-64 AVX has 16, so R=4 fits where the 8 that
/// AArch64's 32 registers allow would spill.
TTTRLIB_TARGET_AVX_FMA
static void fconv_per_avx_impl(double *fit, double *x, double *lamp, int numexp,
                               int start, int stop, int n_points, double period,
                               double dt) {
    if (is_verbose()) {
        std::clog << "FCONV_PER_AVX" << std::endl;
    }
    if (numexp <= 4)
        fconv_per_avx_block<1>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
    else if (numexp <= 8)
        fconv_per_avx_block<2>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
    else
        fconv_per_avx_block<4>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
}
#endif // TTTRLIB_COMPILE_AVX

#if TTTRLIB_COMPILE_NEON
// NEON kernel for fconv_per(): 2 lifetimes per float64x2_t (mirror of the AVX
// kernel). Wins on AArch64 because the per-lifetime recurrence cannot be
// autovectorized.
// Periodic convolution, NEON: `R` registers advanced together, so 2R species
// are in flight at once.
//
// The first version of this kernel put two species in one register and did the
// whole job a pair at a time. That is the natural way to write it and it was
// *slower than plain scalar code*: the recursion `fitcurr = fitcurr*e + l2[i]`
// is a serial dependency chain, one register is one chain, and an FMA takes
// several cycles to retire -- so the machine sat waiting on a latency it had
// no other work to hide. It also paid a cross-lane reduction and a
// read-modify-write of `fit[]` per species-pair per channel, which for 33
// species is 17 passes over the whole array.
//
// So: R independent chains to fill the pipeline, the per-species contributions
// summed in a vector accumulator, and *one* horizontal add and one `fit[]`
// update per channel per block. Measured at 1563 channels on Apple silicon,
// against the pair-at-a-time version it replaces: 4.2x at 16 and 33 species,
// 6.0x at 64. It is also 1.6-1.9x faster than `fconv_per_cs_ad<double>`, the
// blocked scalar AD kernel, which had been beating the old SIMD one outright.
//
// The block width is chosen per call below, because a spectrum of two species
// cannot fill eight registers and pays for the ones it leaves empty.
template <int R>
static void fconv_per_neon_block(double *fit, double *x, double *lamp, int numexp,
                                 int start, int stop, int n_points, double period,
                                 double dt) {
    constexpr int kSpecies = 2 * R;
    const int start1 = std::max(1, start);
    stop = (stop < 0) ? n_points : stop;
    const int n_ele = ((numexp + kSpecies - 1) / kSpecies) * kSpecies;

    const int period_n = (int)ceil(period / dt - 0.5);
    int lamp_start = 0;
    while (lamp_start < stop && lamp[lamp_start++] == 0);
    const int stop1 = std::min(period_n + lamp_start, n_points);

    // Sized by n_points, not by `stop`: the recursion runs to `stop1`, which is
    // bounded by the point count and not by `stop`. See the module README --
    // undersizing this read past the end and imitated a stateful function.
    std::vector<double> l2(n_points), ex(n_ele, 0.0), p(n_ele, 0.0),
                        scale(n_ele, 0.0), tails(n_ele, 0.0);
    for (int i = 0; i < n_points; i++) l2[i] = dt * 0.5 * lamp[i];
    for (int i = 0; i < numexp; i++) {
        ex[i] = exp(-dt / x[2 * i + 1]);
        p[i] = x[2 * i];
        scale[i] = exp(-(period_n - stop1 + start) * dt / x[2 * i + 1]);
        tails[i] = 1. / (1. - exp(-period / x[2 * i + 1]));
    }
    // The padding lanes carry a zero amplitude, so they contribute exactly
    // nothing and need no branch in the inner loop.

    std::fill(fit, fit + n_points, 0.0);
    for (int base = 0; base < numexp; base += kSpecies) {
        float64x2_t e[R], a[R], sc[R], t[R], fitcurr[R];
        for (int r = 0; r < R; ++r) {
            e[r] = vld1q_f64(&ex[base + 2 * r]);
            a[r] = vld1q_f64(&p[base + 2 * r]);
            sc[r] = vld1q_f64(&scale[base + 2 * r]);
            t[r] = vld1q_f64(&tails[base + 2 * r]);
            fitcurr[r] = vdupq_n_f64(0.0);
        }
        {
            const float64x2_t l0 = vdupq_n_f64(l2[0]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) acc = vfmaq_f64(acc, l0, a[r]);
            fit[0] += vaddvq_f64(acc);
        }
        for (int i = start1; i < stop1; ++i) {
            const float64x2_t lo = vdupq_n_f64(l2[i - 1]);
            const float64x2_t hi = vdupq_n_f64(l2[i]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = vfmaq_f64(hi, vaddq_f64(fitcurr[r], lo), e[r]);
                acc = vfmaq_f64(acc, fitcurr[r], a[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
        for (int r = 0; r < R; ++r) fitcurr[r] = vmulq_f64(fitcurr[r], sc[r]);
        for (int i = start; i < stop; ++i) {
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = vmulq_f64(fitcurr[r], e[r]);
                acc = vfmaq_f64(acc, vmulq_f64(fitcurr[r], a[r]), t[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
    }
}

/// Block width by species count: the smallest that holds the whole spectrum,
/// capped at 8 registers. Each was the fastest of the widths measured at its
/// own species count -- a narrow spectrum in a wide block pays for the empty
/// lanes, and past 16 species there is no more latency left to hide.
static void fconv_per_neon_impl(double *fit, double *x, double *lamp, int numexp,
                                int start, int stop, int n_points, double period,
                                double dt) {
    if (is_verbose()) {
        std::clog << "FCONV_PER_NEON" << std::endl;
    }
    if (numexp <= 4)
        fconv_per_neon_block<2>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
    else if (numexp <= 8)
        fconv_per_neon_block<4>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
    else
        fconv_per_neon_block<8>(fit, x, lamp, numexp, start, stop, n_points, period, dt);
}
#endif // TTTRLIB_COMPILE_NEON

// fast convolution, high repetition rate - picks the best available kernel
// automatically; see fconv() for why the choice depends on numexp as well as
// on the host CPU.
void fconv_per(double *fit, double *x, double *lamp, int numexp, int start, int stop,
               int n_points, double period, double dt) {
    if (simd_convolution_available(numexp)) {
#if TTTRLIB_COMPILE_AVX
        fconv_per_avx_impl(fit, x, lamp, numexp, start, stop, n_points, period, dt);
        return;
#elif TTTRLIB_COMPILE_NEON
        fconv_per_neon_impl(fit, x, lamp, numexp, start, stop, n_points, period, dt);
        return;
#endif
    }
    fconv_per_scalar(fit, x, lamp, numexp, start, stop, n_points, period, dt);
}

/// Deprecated alias of fconv_per(); see fconv_simd() above.
void fconv_per_simd(double *fit, double *x, double *lamp, int numexp, int start, int stop,
                   int n_points, double period, double dt) {
    fconv_per(fit, x, lamp, numexp, start, stop, n_points, period, dt);
}


#if TTTRLIB_COMPILE_NEON
// Periodic convolution WITH a convolution stop, vectorised over lifetimes
// (lane 0 and lane 1 carry two different lifetimes of the same spectrum).
// Mirrors fconv_per_cs()'s scalar recurrences exactly; padding lanes are given
// a zero amplitude and a zero decay factor so they contribute nothing.
// Periodic convolution with a convolution stop, NEON: `R` registers, so 2R
// species advance together. Same reasoning as `fconv_per_neon_block` above --
// one register is one dependency chain and the recursion is latency-bound, so
// a pair at a time leaves the machine waiting; and a horizontal reduction plus
// a read-modify-write of `fit[]` per species-pair per channel is many passes
// over the array where one will do.
template <int R>
static void fconv_per_cs_neon_block(double *fit, double *x, double *lamp, int numexp,
                                    int stop, int n_points, double period,
                                    int conv_stop, double dt)
{
    constexpr int kSpecies = 2 * R;
    const int n_ele = ((numexp + kSpecies - 1) / kSpecies) * kSpecies;
    const int period_n = (int)ceil(period / dt - 0.5);
    const int stop1 = (period_n > n_points - 1) ? n_points - 1 : period_n;
    const double deltathalf = dt * 0.5;

    std::vector<double> ex(n_ele, 0.0), amp(n_ele, 0.0), tail(n_ele, 0.0), post(n_ele, 0.0);
    for (int i = 0; i < numexp; i++) {
        ex[i]   = exp(-dt / x[2 * i + 1]);
        amp[i]  = x[2 * i];
        tail[i] = 1. / (1. - exp(-period / x[2 * i + 1]));
        post[i] = exp(-(period_n - stop1) * dt / x[2 * i + 1]);
    }

    for (int i = 0; i <= stop; i++) fit[i] = 0.0;

    for (int base = 0; base < numexp; base += kSpecies) {
        float64x2_t e[R], a[R], t[R], s[R], fitcurr[R];
        for (int r = 0; r < R; ++r) {
            e[r] = vld1q_f64(&ex[base + 2 * r]);
            a[r] = vld1q_f64(&amp[base + 2 * r]);
            t[r] = vld1q_f64(&tail[base + 2 * r]);
            s[r] = vld1q_f64(&post[base + 2 * r]);
            fitcurr[r] = vdupq_n_f64(0.0);
        }
        {
            // fit[0] += deltathalf*lamp[0]*(expcurr + 1.)*amp
            const float64x2_t l0 = vdupq_n_f64(deltathalf * lamp[0]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r)
                acc = vfmaq_f64(acc, vmulq_f64(l0, vaddq_f64(e[r], vdupq_n_f64(1.0))), a[r]);
            fit[0] += vaddvq_f64(acc);
        }
        int i = 1;
        for (; i <= conv_stop; ++i) {
            const float64x2_t lo = vdupq_n_f64(deltathalf * lamp[i - 1]);
            const float64x2_t hi = vdupq_n_f64(deltathalf * lamp[i]);
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = vfmaq_f64(hi, vaddq_f64(fitcurr[r], lo), e[r]);
                acc = vfmaq_f64(acc, fitcurr[r], a[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
        for (; i <= stop1; ++i) {
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                fitcurr[r] = vmulq_f64(fitcurr[r], e[r]);
                acc = vfmaq_f64(acc, fitcurr[r], a[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
        for (int r = 0; r < R; ++r) fitcurr[r] = vmulq_f64(fitcurr[r], s[r]);
        // The wrap-around tail. fitcurr now holds the continuation at bin
        // period_n, which *is* bin 0 of the next period -- so bin 0 takes it as
        // it stands and the decay step comes after, not before. Stepping first
        // would place the value belonging to bin period_n+1 into bin 0 and shift
        // the whole tail one bin early. (fconv_per() gets this right by ending
        // its main loop one bin earlier, which is why only the _cs variants
        // carried the error.) It is invisible whenever the decay completes
        // within the period, and grows as it does not: 5.8e-5 of the peak at a
        // lifetime of a fifth of the period, and larger for longer lifetimes.
        // With this order the recursion matches an exact circular convolution to
        // 1.3e-15; test_dfa_kernel.py pins that against the spectral backend.
        for (i = 0; i <= stop; ++i) {
            float64x2_t acc = vdupq_n_f64(0.0);
            for (int r = 0; r < R; ++r) {
                acc = vfmaq_f64(acc, vmulq_f64(fitcurr[r], a[r]), t[r]);
                fitcurr[r] = vmulq_f64(fitcurr[r], e[r]);
            }
            fit[i] += vaddvq_f64(acc);
        }
    }
}

/// Block width by species count; see `fconv_per_neon_impl` for the measurements.
static void fconv_per_cs_neon_impl(double *fit, double *x, double *lamp, int numexp,
                                   int stop, int n_points, double period,
                                   int conv_stop, double dt)
{
    if (numexp <= 4)
        fconv_per_cs_neon_block<2>(fit, x, lamp, numexp, stop, n_points, period, conv_stop, dt);
    else if (numexp <= 8)
        fconv_per_cs_neon_block<4>(fit, x, lamp, numexp, stop, n_points, period, conv_stop, dt);
    else
        fconv_per_cs_neon_block<8>(fit, x, lamp, numexp, stop, n_points, period, conv_stop, dt);
}
#endif // TTTRLIB_COMPILE_NEON

/* fast convolution, high repetition rate, with convolution stop for Paris */
static void fconv_per_cs_scalar(double *fit, double *x, double *lamp, int numexp, int stop,
                  int n_points, double period, int conv_stop, double dt)
{
    int ne, i,
            stop1, period_n = (int)ceil(period/dt-0.5);
    double fitcurr, expcurr, tail_a, deltathalf = dt*0.5;

    for (i=0; i<=stop; i++) fit[i]=0;
    stop1 = (period_n > n_points-1) ? n_points-1 : period_n;

    /* convolution */
    for (ne=0; ne<numexp; ne++) {
        expcurr = exp(-dt/x[2*ne+1]);
        tail_a = 1./(1.-exp(-period/x[2*ne+1]));
        fitcurr = 0.;
        fit[0] += deltathalf*lamp[0]*(expcurr + 1.)*x[2*ne];
        for (i=1; i<=conv_stop; i++) {
            fitcurr=(fitcurr + deltathalf*lamp[i-1])*expcurr + deltathalf*lamp[i];
            fit[i] += fitcurr*x[2*ne];
        }
        for (; i<=stop1; i++) {
            fitcurr=fitcurr*expcurr;
            fit[i] += fitcurr*x[2*ne];
        }
        fitcurr *= exp(-(period_n - stop1)*dt/x[2*ne+1]);
        // The wrap-around tail. fitcurr now holds the continuation at bin
        // period_n, which *is* bin 0 of the next period — so bin 0 takes it as
        // it stands and the decay step comes after, not before. Stepping first
        // would place the value belonging to bin period_n+1 into bin 0 and shift
        // the whole tail one bin early. (fconv_per() gets this right by ending
        // its main loop one bin earlier, which is why only the _cs variants
        // carried the error.) It is invisible whenever the decay completes
        // within the period, and grows as it does not: 5.8e-5 of the peak at a
        // lifetime of a fifth of the period, and larger for longer lifetimes.
        // With this order the recursion matches an exact circular convolution to
        // 1.3e-15; test_dfa_kernel.py pins that against the spectral backend.
        for (i=0; i<=stop; i++) {
            fit[i] += fitcurr*x[2*ne]*tail_a;
            fitcurr *= expcurr;
        }
    }
}

// Periodic convolution with a convolution stop - picks the best available
// kernel automatically, on the same CPU-and-size rule as fconv()/fconv_per().
// No AVX kernel exists for this variant yet, so x86 takes the scalar path.
// Model curve and exact Jacobian in one forward-mode pass per block of
// parameters. See the header for the column order and why it is blocked.
void fconv_per_cs_jacobian(
        double *fit, int n_fit,
        double *jacobian, int n_jac1, int n_jac2,
        double *x, int n_x,
        double *lamp, int n_lamp,
        double period, double time_shift, int conv_stop, int stop, double dt) {
    if (n_lamp != n_fit)
        throw std::invalid_argument(
            "fconv_per_cs_jacobian: the response and the model must have the "
            "same number of points");
    if (n_x < 2 || (n_x % 2) != 0)
        throw std::invalid_argument(
            "fconv_per_cs_jacobian: the lifetime spectrum is (amplitude, "
            "lifetime) pairs, so its length must be even and at least 2");
    const int n_params = n_x + 1;              // + the timeshift
    if (n_jac1 != n_fit || n_jac2 != n_params)
        throw std::invalid_argument(
            "fconv_per_cs_jacobian: the jacobian must be (n_points, n_x + 1) "
            "-- one row per channel, one column per spectrum entry plus a last "
            "column for the timeshift");

    const int numexp = n_x / 2;
    const int n_points = n_fit;
    if (stop < 0) stop = n_points - 1;

    using Grad = tttrlib::GradVec<FCONV_JAC_BLOCK>;
    using D = tttrlib::Dual<Grad>;

    std::vector<D> xd(static_cast<size_t>(n_x));
    std::vector<D> lampsh(static_cast<size_t>(n_points));
    std::vector<D> fitd(static_cast<size_t>(n_points));

    for (int base = 0; base < n_params; base += FCONV_JAC_BLOCK) {
        // Seed this block's directions. Everything outside it is a constant in
        // this pass, which is what makes the pass cost one value evaluation
        // plus B derivatives rather than n_params of them.
        for (int j = 0; j < n_x; ++j) {
            const int lane = j - base;
            xd[static_cast<size_t>(j)] =
                (lane >= 0 && lane < FCONV_JAC_BLOCK) ? D(x[j], Grad::Unit(lane))
                                                      : D(x[j]);
        }
        const int shift_lane = n_x - base;
        const D ts = (shift_lane >= 0 && shift_lane < FCONV_JAC_BLOCK)
                         ? D(time_shift, Grad::Unit(shift_lane))
                         : D(time_shift);

        shift_lamp_ad<D>(lampsh.data(), lamp, ts, n_points);
        fconv_per_cs_ad<D, D>(fitd.data(), xd.data(), lampsh.data(), numexp,
                              stop, n_points, period, conv_stop, dt);

        const int n_lanes = std::min(FCONV_JAC_BLOCK, n_params - base);
        for (int i = 0; i < n_points; ++i)
            for (int lane = 0; lane < n_lanes; ++lane)
                jacobian[static_cast<size_t>(i) * n_params + base + lane] =
                    fitd[static_cast<size_t>(i)].grad[lane];
    }

    // The value is the same in every pass; take it from the last one.
    for (int i = 0; i < n_points; ++i) fit[i] = fitd[static_cast<size_t>(i)].val;
}


void fconv_per_cs(double *fit, double *x, double *lamp, int numexp, int stop,
                  int n_points, double period, int conv_stop, double dt)
{
#if TTTRLIB_COMPILE_NEON
    if (numexp >= kSimdMinNumexp && tttrlib::cpu_features::get_neon_enabled()) {
        fconv_per_cs_neon_impl(fit, x, lamp, numexp, stop, n_points, period, conv_stop, dt);
        return;
    }
#endif
    fconv_per_cs_scalar(fit, x, lamp, numexp, stop, n_points, period, conv_stop, dt);
}


#if TTTRLIB_COMPILE_NEON
// Two channels (lane 0 = channel 0, lane 1 = channel 1) of the periodic
// convolution in NEON float64x2. The channels share the lifetimes (so expcurr
// is a broadcast scalar); only the amplitudes and IRF differ per lane. FMA is
// used (as in fconv_neon_impl), so results match the scalar path to rounding.
static void fconv_per_cs_2ch_neon(
        double *fit0, double *fit1,
        const double *x0, const double *x1,
        const double *lamp0, const double *lamp1,
        int numexp, int stop, int n_points,
        double period, int conv_stop, double dt) {
    const int period_n = (int)ceil(period / dt - 0.5);
    const double dh = dt * 0.5;
    for (int i = 0; i <= stop; i++) { fit0[i] = 0.0; fit1[i] = 0.0; }
    const int stop1 = (period_n > n_points - 1) ? n_points - 1 : period_n;
    const float64x2_t vdh = vdupq_n_f64(dh);
    const float64x2_t vone = vdupq_n_f64(1.0);
    for (int ne = 0; ne < numexp; ne++) {
        const double lifetime = x0[2 * ne + 1];  // shared with x1[2*ne+1]
        const double expcurr = exp(-dt / lifetime);
        const double tail_a = 1.0 / (1.0 - exp(-period / lifetime));
        const float64x2_t ve = vdupq_n_f64(expcurr);
        const float64x2_t vamp = float64x2_t{x0[2 * ne], x1[2 * ne]};
        // fit[0] += dh*lamp[0]*(expcurr + 1)*amp
        float64x2_t vf = float64x2_t{fit0[0], fit1[0]};
        float64x2_t vl = float64x2_t{lamp0[0], lamp1[0]};
        vf = vfmaq_f64(vf, vmulq_f64(vmulq_f64(vdh, vl), vaddq_f64(ve, vone)), vamp);
        fit0[0] = vgetq_lane_f64(vf, 0); fit1[0] = vgetq_lane_f64(vf, 1);
        float64x2_t vfc = vdupq_n_f64(0.0);
        int i;
        for (i = 1; i <= conv_stop; i++) {
            const float64x2_t vlm1 = float64x2_t{lamp0[i - 1], lamp1[i - 1]};
            const float64x2_t vli = float64x2_t{lamp0[i], lamp1[i]};
            vfc = vaddq_f64(vfc, vmulq_f64(vdh, vlm1));   // fitcurr + dh*lamp[i-1]
            vfc = vfmaq_f64(vmulq_f64(vdh, vli), vfc, ve); // *expcurr + dh*lamp[i]
            vf = float64x2_t{fit0[i], fit1[i]};
            vf = vfmaq_f64(vf, vfc, vamp);
            fit0[i] = vgetq_lane_f64(vf, 0); fit1[i] = vgetq_lane_f64(vf, 1);
        }
        for (; i <= stop1; i++) {
            vfc = vmulq_f64(vfc, ve);
            vf = float64x2_t{fit0[i], fit1[i]};
            vf = vfmaq_f64(vf, vfc, vamp);
            fit0[i] = vgetq_lane_f64(vf, 0); fit1[i] = vgetq_lane_f64(vf, 1);
        }
        vfc = vmulq_f64(vfc, vdupq_n_f64(exp(-(period_n - stop1) * dt / lifetime)));
        const float64x2_t vtail = vdupq_n_f64(tail_a);
        // The wrap-around tail. fitcurr now holds the continuation at bin
        // period_n, which *is* bin 0 of the next period — so bin 0 takes it as
        // it stands and the decay step comes after, not before. Stepping first
        // would place the value belonging to bin period_n+1 into bin 0 and shift
        // the whole tail one bin early. (fconv_per() gets this right by ending
        // its main loop one bin earlier, which is why only the _cs variants
        // carried the error.) It is invisible whenever the decay completes
        // within the period, and grows as it does not: 5.8e-5 of the peak at a
        // lifetime of a fifth of the period, and larger for longer lifetimes.
        // With this order the recursion matches an exact circular convolution to
        // 1.3e-15; test_dfa_kernel.py pins that against the spectral backend.
        for (i = 0; i <= stop; i++) {
            vf = float64x2_t{fit0[i], fit1[i]};
            vf = vfmaq_f64(vf, vmulq_f64(vfc, vamp), vtail);
            fit0[i] = vgetq_lane_f64(vf, 0); fit1[i] = vgetq_lane_f64(vf, 1);
            vfc = vmulq_f64(vfc, ve);
        }
    }
}
#endif // TTTRLIB_COMPILE_NEON


void fconv_per_cs_2ch(double *fit0, double *fit1,
                      const double *x0, const double *x1,
                      const double *lamp0, const double *lamp1,
                      int numexp, int stop, int n_points,
                      double period, int conv_stop, double dt) {
#if TTTRLIB_COMPILE_NEON
    if (tttrlib::cpu_features::get_neon_enabled()) {
        fconv_per_cs_2ch_neon(fit0, fit1, x0, x1, lamp0, lamp1,
                              numexp, stop, n_points, period, conv_stop, dt);
        return;
    }
#endif
    // Scalar fallback: two independent single-channel convolutions.
    fconv_per_cs(fit0, const_cast<double *>(x0), const_cast<double *>(lamp0),
                 numexp, stop, n_points, period, conv_stop, dt);
    fconv_per_cs(fit1, const_cast<double *>(x1), const_cast<double *>(lamp1),
                 numexp, stop, n_points, period, conv_stop, dt);
}


/* fast convolution with reference compound decay */
void fconv_ref(double *fit, double *x, double *lamp, int numexp, int start, int stop, double tauref, double dt) {
    double deltathalf = dt * 0.5, sum_a = 0;
    for (int i = 0; i < stop; i++) fit[i] = 0;
    /* convolution */
    for (int ne = 0; ne < numexp; ne++) {
        double expcurr = exp(-dt / x[2 * ne + 1]);
        double correct_a = x[2 * ne] * (1 / tauref - 1 / x[2 * ne + 1]);
        sum_a += x[2 * ne];
        double fitcurr = 0;
        for (int i = 1; i < stop; i++) {
            fitcurr = (fitcurr + deltathalf * lamp[i - 1]) * expcurr + deltathalf * lamp[i];
            fit[i] += fitcurr * correct_a;
        }
    }
    for (int i = 1; i < stop; i++) fit[i] += lamp[i] * sum_a;
}

/* slow convolution */
void sconv(double *fit, double *p, double *lamp, int start, int stop) {
    int i, j;
    /* convolution */
    for (i = start; i < stop; i++) {
        fit[i] = 0.5 * lamp[0] * p[i];
        for (j = 1; j < i; j++) fit[i] += lamp[j] * p[i - j];
        fit[i] += 0.5 * lamp[i] * p[0];
        fit[i] = fit[i];
    }
    fit[0] = 0;
}


/* shifting lamp */
void shift_lamp(double *lampsh, double *lamp, double ts, int n_points, double out_value) {
    // The body is `shift_lamp_ad<double>` in the header, so that a consumer
    // holding only the header -- imp.bff's TCSPC decay node, where the
    // timeshift is a fit parameter -- shifts a response function with this
    // implementation rather than a second one of its own.
    shift_lamp_ad<double>(lampsh, lamp, ts, n_points, out_value);
}


void add_pile_up_to_model(
        double* model, int n_model,
        double* data, int n_data,
        double repetition_rate,
        double instrument_dead_time,
        double measurement_time,
        std::string pile_up_model,
        int start,
        int stop
){
    // Window clamping lives in the template. (The line this replaces,
    // `stop = std::min(n_data, n_model);`, unconditionally overwrote a
    // caller's stop -- the parameter was silently ignored.)
    stop = stop < 0 ? n_data : std::min(n_data, stop);
    start = start < 0 ? 0 : std::min(n_data, start);
if (is_verbose()) {
    std::clog << "ADD PILE-UP" << std::endl;
    std::clog << "-- Repetition_rate [MHz]: " << repetition_rate << std::endl;
    std::clog << "-- Dead_time [ns]: " << instrument_dead_time << std::endl;
    std::clog << "-- Measurement_time [s]: " << measurement_time << std::endl;
    std::clog << "-- n_data: " << n_data << std::endl;
    std::clog << "-- n_model: " << n_model << std::endl;
    std::clog << "-- start: " << start << std::endl;
    std::clog << "-- stop: " << stop << std::endl;
}
    if(strcmp(pile_up_model.c_str(), "coates") == 0){
if (is_verbose()) {
        std::clog << "-- pile_up_model: " << pile_up_model << std::endl;
}
        // The body is `add_pile_up_to_model_ad<double>` in the header, so a
        // consumer holding only the header -- imp.bff's TCSPC decay node --
        // applies pile-up with this implementation rather than a second one
        // of its own. The template also carries chisurf's edge-case
        // semantics: a too-short measurement time leaves the model unscaled
        // instead of producing NaN, the detection probability is capped
        // strictly below one, and an empty channel takes the analytic p->0
        // limit instead of forcing the model to zero there.
        add_pile_up_to_model_ad<double>(
            model, n_model, data, n_data,
            repetition_rate, instrument_dead_time, measurement_time,
            start, stop);
    }
}


void discriminate_small_amplitudes(
        double* lifetime_spectrum, int n_lifetime_spectrum,
        double amplitude_threshold
){
    int number_of_exponentials = n_lifetime_spectrum / 2;
if (is_verbose()) {
    std::clog << "APPLY_AMPLITUDE_THRESHOLD" << std::endl;
    std::clog << "-- amplitude_threshold spectrum: " << amplitude_threshold << std::endl;
    std::clog << "-- lifetime spectrum before: ";
    for (int i=0; i < number_of_exponentials * 2; i++){
        std::clog << lifetime_spectrum[i] << ' ';
    }
    std::clog << std::endl;
}
    for(int ne = 0; ne<number_of_exponentials; ne++){
        double amplitude = lifetime_spectrum[2 * ne];
        if(std::abs(amplitude) < amplitude_threshold){
            lifetime_spectrum[2 * ne] = 0.0;
        }
    }
if (is_verbose()) {
    std::clog << "-- lifetime spectrum after: ";
    for (int i=0; i < number_of_exponentials * 2; i++){
        std::clog << lifetime_spectrum[i] << ' ';
    }
    std::clog << std::endl;
}
}


/* fast convolution, high repetition rate, with time axis */
void fconv_per_cs_time_axis(
        double* model, int n_model,
        double* time_axis, int n_time_axis,
        double *irf, int n_irf,
        double* lifetime_spectrum, int n_lifetime_spectrum,
        int convolution_start,
        int convolution_stop,
        double period
){
    double dt = time_axis[1] - time_axis[0];
    // fconv_per() dispatches to a SIMD kernel when the CPU supports one and
    // falls back to the scalar path otherwise.
    fconv_per(
            model, lifetime_spectrum, irf, (int) n_lifetime_spectrum / 2,
            convolution_start, convolution_stop, n_model, period, dt
    );
}


/* fast convolution, high repetition rate, with time axis */
void fconv_cs_time_axis(
        double* output, int n_output,
        double* time_axis, int n_time_axis,
        double *irf, int n_irf,
        double* lifetime_spectrum, int n_lifetime_spectrum,
        int convolution_start,
        int convolution_stop
){
    double dt = time_axis[1] - time_axis[0];
    // fconv() dispatches to a SIMD kernel when the CPU supports one and
    // falls back to the scalar path otherwise.
    fconv(
            output,
            lifetime_spectrum,
            irf,
            (int) n_lifetime_spectrum / 2,
            convolution_start, convolution_stop, dt
    );
}


void fconv_cs_time_axis_old(
        double* output, int n_output,
        double* time_axis, int n_time_axis,
        double *irf, int n_irf,
        double* lifetime_spectrum, int n_lifetime_spectrum,
        int convolution_start,
        int convolution_stop
){
    int number_of_exponentials = n_lifetime_spectrum / 2;
if (is_verbose()) {
    std::clog << "convolve_lifetime_spectrum... " << std::endl;
    std::clog << "-- number_of_exponentials: " << number_of_exponentials << std::endl;
    std::clog << "-- convolution_start: " << convolution_start << std::endl;
    std::clog << "-- convolution_stop: " << convolution_stop << std::endl;
}
    for(int ne=0; ne<number_of_exponentials; ne++){
        double a = lifetime_spectrum[2 * ne];
        double current_lifetime = (lifetime_spectrum[2 * ne + 1]);
        if((a == 0.0) || (current_lifetime == 0.0)) continue;
        double current_model_value = 0.0;
        for(int i = convolution_start; i < convolution_stop; i++){
            double dt;
            int pre = std::max(0, i - 1);
            if(i < convolution_stop - 1){
                dt = (time_axis[i + 1] - time_axis[i]);
            } else{
                dt = (time_axis[i] - time_axis[i - 1]);
            }
            double dt_2 = dt / 2.0;
            double current_exponential = std::exp(-dt / current_lifetime);
            current_model_value = (current_model_value + dt_2 * irf[pre]) *
                                  current_exponential + dt_2 * irf[i];
            output[i] += current_model_value * a;
        }
    }
}

// ---- registry entries (Registry.h, core): declared next to the code, registered
// when this library loads; a static consumer links the archive whole.
namespace {
const char* const kDecayConvolutionEntry = R"JSON({
  "name": "decay_convolution",
  "label": "Decay-model kernels: IRF convolution, periodicity, pile-up, lamp shift and rescaling",
  "summary": "The building blocks every decay model is assembled from: convolution of a lifetime spectrum with the IRF (single-shot and periodic, per-channel, SIMD), pile-up correction, lamp shift, and scaling to the data.",
  "description": "`fconv` and its periodic / per-channel / SIMD variants compute the reconvolution integral of an exponential lifetime spectrum with the measured IRF on the micro-time axis (Fit2x's kernels), `dfa_*` the anisotropy (VV/VH) forms, `add_pile_up_to_model` Coates' pile-up correction, `shift_lamp` a fractional-channel IRF shift, `rescale*` the maximum-likelihood scaling of a model to counts with or without a background pattern. These are the primitives; the models in the `fit` category compose them.",
  "operation_type": "analysis",
  "method": "fconv",
  "params_schema": {
    "type": "object",
    "properties": {
      "dt": {
        "type": "number",
        "title": "Bin width (ns)",
        "default": 1.0
      },
      "period": {
        "type": "number",
        "title": "Period (ns)"
      },
      "start": {
        "type": "integer",
        "title": "Start channel",
        "default": 0
      },
      "stop": {
        "type": "integer",
        "title": "Stop channel",
        "default": -1
      }
    }
  },
  "inputs": {
    "required": [
      "lifetime_spectrum",
      "irf"
    ]
  },
  "outputs": {
    "columns": [
      "model_decay"
    ]
  },
  "row_grain": "curve_point",
  "references": [
    {
      "type": "book",
      "authors": "O'Connor, D. V., Phillips, D.",
      "title": "Time-correlated Single Photon Counting",
      "publisher": "Academic Press",
      "year": 1984
    },
    {
      "type": "journal",
      "authors": "Coates, P. B.",
      "title": "The correction for photon 'pile-up' in the measurement of radiative lifetimes",
      "journal": "J Phys E",
      "year": 1968,
      "volume": "1",
      "pages": "878-879"
    }
  ],
  "api": [
    "fconv",
    "fconv_cs_time_axis",
    "fconv_per",
    "fconv_per_cs",
    "fconv_per_cs_2ch",
    "fconv_per_cs_jacobian",
    "fconv_per_cs_time_axis",
    "fconv_per_simd",
    "fconv_ref",
    "fconv_simd",
    "sconv",
    "dfa_convolve",
    "dfa_convolved_decay",
    "dfa_periodic_decay",
    "dfa_vv_vh_convolved",
    "dfa_vv_vh_decay",
    "add_pile_up_to_model",
    "shift_lamp",
    "rescale",
    "rescale_w",
    "rescale_w_bg",
    "discriminate_small_amplitudes",
    "decay_fit23_model_curve"
  ],
  "can_replay": false
})JSON";
bool register_decayconvolution_entries() {
    tttrlib::register_algorithm_json("decay", "decay_convolution", kDecayConvolutionEntry);
    return true;
}
const bool kDecayConvolutionRegistered = register_decayconvolution_entries();
}  // namespace
