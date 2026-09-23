// SPDX-License-Identifier: BSD-3-Clause
//
// A/B test: DriftEstimator.h against known synthetic translations.
//
//   c++ -std=c++17 -O2 -I modules/math/include test/cpp/test_drift_estimator.cpp \
//       -o /tmp/test_drift_estimator && /tmp/test_drift_estimator
//
// Exit status is the number of failed checks.

#include "DriftEstimator.h"
#include "FastGaussian.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace tttrlib;
namespace de = tttrlib::drift_estimator;
namespace fg = tttrlib::fast_gaussian;

static int g_failures = 0;

static void report(bool ok, const char* what, double value, double tol) {
    if (!ok) {
        std::printf("  FAIL  %s (%.3g > %.3g)\n", what, value, tol);
        ++g_failures;
    } else {
        std::printf("  ok    %s (%.3g)\n", what, value);
    }
}

struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    double next(double lo, double hi) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double u = static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) /
                         static_cast<double>(1ULL << 53);
        return lo + (hi - lo) * u;
    }
};

static std::vector<double> make_texture(int rows, int cols, Lcg& rng) {
    std::vector<double> im(static_cast<size_t>(rows) * cols);
    for (double& v : im) v = rng.next(0, 1);
    std::vector<double> smooth(im.size());
    fg::gaussian_blur_fast(im.data(), smooth.data(), rows, cols, 2.5);
    return smooth;
}

/// img(r, c) = ref(r - ty, c - tx), bilinear -- the translation the estimator
/// must report is (tx, ty).
static std::vector<double> translate(const std::vector<double>& ref, int rows, int cols,
                                     double tx, double ty) {
    std::vector<double> out(static_cast<size_t>(rows) * cols);
    for (int r = 0; r < rows; ++r) {
        const double sr = r - ty;
        const int y0 = std::min(std::max(static_cast<int>(std::floor(sr)), 0), rows - 1);
        const int y1 = std::min(y0 + 1, rows - 1);
        const double ay = std::min(std::max(sr - y0, 0.0), 1.0);
        for (int c = 0; c < cols; ++c) {
            const double sc = c - tx;
            const int x0 = std::min(std::max(static_cast<int>(std::floor(sc)), 0), cols - 1);
            const int x1 = std::min(x0 + 1, cols - 1);
            const double ax = std::min(std::max(sc - x0, 0.0), 1.0);
            const double top = ref[static_cast<size_t>(y0) * cols + x0] * (1 - ax) + ref[static_cast<size_t>(y0) * cols + x1] * ax;
            const double bot = ref[static_cast<size_t>(y1) * cols + x0] * (1 - ax) + ref[static_cast<size_t>(y1) * cols + x1] * ax;
            out[static_cast<size_t>(r) * cols + c] = top * (1 - ay) + bot * ay;
        }
    }
    return out;
}

static void test_integer_shifts() {
    std::printf("integer shifts\n");
    const int rows = 192, cols = 160;
    Lcg rng(51);
    const std::vector<double> ref = make_texture(rows, cols, rng);
    double worst = 0.0;
    for (int ty = -7; ty <= 7; ty += 2)
        for (int tx = -7; tx <= 7; tx += 2) {
            if (tx == 0 && ty == 0) continue;
            const std::vector<double> img = translate(ref, rows, cols, tx, ty);
            double dx, dy, score;
            de::estimate_shift(ref.data(), img.data(), rows, cols, 10, dx, dy, score);
            worst = std::max(worst, std::hypot(dx - tx, dy - ty));
        }
    report(worst <= 0.30, "integer shifts recovered within 0.30 px", worst, 0.30);
}

static void test_subpixel_shifts() {
    std::printf("sub-pixel shifts\n");
    const int rows = 192, cols = 160;
    Lcg rng(52);
    const std::vector<double> ref = make_texture(rows, cols, rng);
    double worst = 0.0;
    for (double ty = -3.0; ty <= 3.0; ty += 0.5)
        for (double tx = -3.0; tx <= 3.0; tx += 0.5) {
            if (tx == 0.0 && ty == 0.0) continue;
            const std::vector<double> img = translate(ref, rows, cols, tx, ty);
            double dx, dy, score;
            de::estimate_shift(ref.data(), img.data(), rows, cols, 6, dx, dy, score);
            worst = std::max(worst, std::hypot(dx - tx, dy - ty));
        }
    report(worst <= 0.30, "sub-pixel shifts recovered within 0.30 px", worst, 0.30);
}

static void test_degenerate() {
    std::printf("degenerate inputs\n");
    const int rows = 96, cols = 80;
    std::vector<double> flat(static_cast<size_t>(rows) * cols, 3.0);
    double dx = 99, dy = 99, score = 99;
    de::estimate_shift(flat.data(), flat.data(), rows, cols, 8, dx, dy, score);
    report(dx == 0.0 && dy == 0.0 && score == 0.0, "flat pair reports no shift, score 0",
           std::abs(dx) + std::abs(dy) + std::abs(score), 0.0);

    Lcg rng(53);
    const std::vector<double> ref = make_texture(rows, cols, rng);
    const std::vector<double> img = translate(ref, rows, cols, 6, -6);
    bool threw = false;
    try {
        de::estimate_shift(ref.data(), img.data(), rows, cols, 0, dx, dy, score);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    report(threw, "max_shift < 1 throws", 0.0, 0.0);
}

int main() {
    test_integer_shifts();
    test_subpixel_shifts();
    test_degenerate();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
