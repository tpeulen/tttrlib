// SPDX-License-Identifier: BSD-3-Clause
//
// A/B benchmark: the convolution kernels against each other.
//
// The question this exists to answer is "is the optimized path actually the
// fastest path", which for a while it was not. The hand-written SIMD kernels
// put 2 (NEON) or 4 (AVX) species in one register, which is a single dependency
// chain against the FMA latency, and reduced it to a scalar once per channel
// *per species-group*. Blocked scalar code beat them outright. They now advance
// several registers at once and reduce once per channel per block; this
// measures that, and will show it if anyone regresses it.
//
// The last column is the yardstick: `fconv_per_cs_ad<double>` is the AD kernel's
// own scalar instantiation, blocked eight species at a time. If a hand-written
// SIMD kernel is not beating it, the SIMD kernel is the thing to fix.
//
// Build (from the repo root):
//   clang++ -std=c++17 -O3 \
//     $(sed -n 's/^CXX_INCLUDES = //p' \
//        build_new/modules/math/CMakeFiles/tttrlib_math.dir/flags.make) \
//     -I modules/spectroscopy/decay/include \
//     benchmarks/bench_convolution_kernels.cpp \
//     modules/spectroscopy/decay/src/DecayConvolution.cpp \
//     -L build_new -ltttrlib_static -o benchmarks/bench_convolution_kernels
//
// Run it twice to see both dispatch paths -- the scalar fallback is selected
// through the same environment override the tests use:
//   benchmarks/bench_convolution_kernels
//   TTTRLIB_USE_NEON=0 TTTRLIB_USE_AVX=0 benchmarks/bench_convolution_kernels
//
// Timing follows benchmarks/README.md: CLOCK_THREAD_CPUTIME_ID, minimum over
// nine trials. Wall clock keeps counting while the thread is descheduled and
// reported speedups between 0.22x and 4.77x for one binary on a loaded machine.

#include "DecayConvolution.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

namespace {

double thread_cpu_seconds() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec + 1e-9 * ts.tv_nsec;
}

template <typename F>
double best_ms(F f, int reps = 200, int trials = 9) {
    f();  // warm
    double best = 1e30;
    for (int t = 0; t < trials; ++t) {
        const double t0 = thread_cpu_seconds();
        for (int r = 0; r < reps; ++r) f();
        best = std::min(best, (thread_cpu_seconds() - t0) / reps);
    }
    return best * 1e3;
}

}  // namespace

int main(int argc, char** argv) {
    const int n_points = argc > 1 ? std::atoi(argv[1]) : 1563;
    const double period = 50.0;
    const double dt = period / n_points;

    std::vector<double> lamp(n_points);
    for (int i = 0; i < n_points; ++i) {
        const double t = (i - 60.0) / 9.0;
        lamp[i] = std::exp(-0.5 * t * t);
    }

    const char* neon = std::getenv("TTTRLIB_USE_NEON");
    const char* avx = std::getenv("TTTRLIB_USE_AVX");
    std::printf("convolution kernels, %d channels, period %.1f\n", n_points, period);
    std::printf("TTTRLIB_USE_NEON=%s TTTRLIB_USE_AVX=%s\n",
                neon ? neon : "(unset)", avx ? avx : "(unset)");
    std::printf("%10s %10s %14s %14s %18s\n",
                "lifetimes", "fconv", "fconv_per", "fconv_per_cs",
                "fconv_per_cs_ad<double>");

    for (int numexp : {1, 2, 4, 8, 16, 33, 64}) {
        std::vector<double> x(2 * numexp), fit(n_points);
        for (int k = 0; k < numexp; ++k) {
            x[2 * k] = 1.0 / numexp;
            x[2 * k + 1] = 0.4 + 4.0 * k / numexp;
        }
        const double plain = best_ms([&] {
            fconv(fit.data(), x.data(), lamp.data(), numexp, 0, n_points, dt);
        });
        const double per = best_ms([&] {
            fconv_per(fit.data(), x.data(), lamp.data(), numexp, 0, n_points,
                      n_points, period, dt);
        });
        const double cs = best_ms([&] {
            fconv_per_cs(fit.data(), x.data(), lamp.data(), numexp, n_points - 1,
                         n_points, period, n_points - 1, dt);
        });
        // The AD kernel at `double`: blocked scalar, and the thing the SIMD
        // kernels have to beat to justify existing.
        const double ad = best_ms([&] {
            fconv_per_cs_ad<double>(fit.data(), x.data(), lamp.data(), numexp,
                                    n_points - 1, n_points, period,
                                    n_points - 1, dt);
        });
        std::printf("%10d %10.4f %14.4f %14.4f %18.4f\n", numexp, plain, per, cs, ad);
    }
    return 0;
}
