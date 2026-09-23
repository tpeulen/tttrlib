// The interleaved fconv_per_cs_ad against the plain recursion it replaced.
//
// `fconv_per_cs_ad` advances FCONV_AD_BLOCK species recursions at once,
// because each species is a serial dependency chain over the channels and one
// at a time leaves the pipeline mostly empty. The arithmetic per species is
// unchanged; what changes is the *order* the per-species contributions are
// summed into `fit[i]` -- a block is summed and added once instead of each
// species being added in turn.
//
// So there are two properties, and they are different:
//
//   1. Below FCONV_AD_BLOCK_MIN there is nothing to interleave and the serial
//      body is used, so the result must be **bit-identical**. That is what
//      keeps every single-exponential decay in the library exactly where it
//      was.
//   2. At or above it, the result must agree to a few ULP -- the callers that
//      pin curves do so at 1e-10 to 1e-14, and this must sit far inside that
//      with room to spare rather than just underneath it.
//
// Header-only, like its neighbours: nothing to link, runnable by hand.
//
//     c++ -std=c++17 -O2 -I modules/spectroscopy/decay/include \
//         -I modules/math/include -o t test/cpp/test_fconv_interleave.cpp && ./t
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "DecayConvolution.h"

static int failures = 0;

static void check(bool ok, const char *what, double detail) {
    std::printf("  %-4s  %-56s (%.3g)\n", ok ? "ok" : "FAIL", what, detail);
    if (!ok) failures++;
}

static const int N = 512;
static const double DT = 0.032;
static const double PERIOD = 1000.0 / 80.0;

static std::vector<double> irf() {
    std::vector<double> lamp(N);
    double total = 0.0;
    for (int i = 0; i < N; i++) {
        const double t = i * DT, z = (t - 1.0) / 0.08;
        lamp[i] = std::exp(-0.5 * z * z);
        total += lamp[i];
    }
    for (int i = 0; i < N; i++) lamp[i] /= total;
    return lamp;
}

// A spectrum whose lifetimes span the range a distance distribution produces:
// a donor at ~4 ns down to a strongly quenched 0.1 ns.
static std::vector<double> spectrum(int numexp) {
    std::vector<double> x(2 * numexp);
    for (int k = 0; k < numexp; k++) {
        x[2 * k] = (1.0 + 0.3 * std::sin((double)k)) / numexp;
        x[2 * k + 1] = 0.1 + 3.9 * (k + 1.0) / numexp;
    }
    return x;
}

int main() {
    const std::vector<double> lamp = irf();

    std::printf("interleaved vs serial, block = %d, threshold = %d\n",
                FCONV_AD_BLOCK, FCONV_AD_BLOCK_MIN);
    for (int numexp : {1, 2, 3, 4, 5, 7, 8, 9, 16, 17, 53, 97}) {
        const std::vector<double> x = spectrum(numexp);
        std::vector<double> a(N, 0.0), b(N, 0.0);
        fconv_per_cs_ad_serial<double>(a.data(), x.data(), lamp.data(), numexp,
                                       N - 1, N, PERIOD, N - 1, DT);
        fconv_per_cs_ad<double>(b.data(), x.data(), lamp.data(), numexp,
                                N - 1, N, PERIOD, N - 1, DT);
        double peak = 0.0, worst = 0.0;
        for (int i = 0; i < N; i++) peak = std::max(peak, std::fabs(a[i]));
        for (int i = 0; i < N; i++) worst = std::max(worst, std::fabs(a[i] - b[i]));
        const double rel = peak > 0.0 ? worst / peak : worst;

        char what[96];
        if (numexp < FCONV_AD_BLOCK_MIN) {
            std::snprintf(what, sizeof what,
                          "numexp %d: below the threshold, bit-identical", numexp);
            check(worst == 0.0, what, rel);
        } else {
            std::snprintf(what, sizeof what,
                          "numexp %d: agrees to a few ULP", numexp);
            // 1e-13 is two orders inside the tightest tolerance any caller
            // pins a curve at, and the measured value is ~5e-16.
            check(rel < 1e-13, what, rel);
        }
        // The curve must not be empty, or the comparison above is vacuous --
        // the failure mode a padding lane could produce is *everything* zero.
        if (peak <= 0.0) check(false, "the reference curve is not identically zero", peak);
    }

    // A block boundary is where a padding lane would show up, so the counts
    // either side of one are checked above (7/8/9, 16/17). This pins the
    // reason: padding lanes must contribute exactly nothing, so appending
    // zero-amplitude species must not change the answer at all.
    {
        const int numexp = 5;
        std::vector<double> x = spectrum(numexp);
        std::vector<double> padded = x;
        for (int k = 0; k < 3; k++) {          // three lanes of nothing
            padded.push_back(0.0);              // amplitude
            padded.push_back(2.0);              // a real lifetime, zero weight
        }
        std::vector<double> a(N, 0.0), b(N, 0.0);
        fconv_per_cs_ad<double>(a.data(), x.data(), lamp.data(), numexp,
                                N - 1, N, PERIOD, N - 1, DT);
        fconv_per_cs_ad<double>(b.data(), padded.data(), lamp.data(), numexp + 3,
                                N - 1, N, PERIOD, N - 1, DT);
        double worst = 0.0, peak = 0.0;
        for (int i = 0; i < N; i++) peak = std::max(peak, std::fabs(a[i]));
        for (int i = 0; i < N; i++) worst = std::max(worst, std::fabs(a[i] - b[i]));
        check(worst / peak < 1e-13,
              "zero-amplitude species change nothing", worst / peak);
    }

    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
                failures);
    return failures ? 1 : 0;
}
