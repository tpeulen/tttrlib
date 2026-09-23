// SPDX-License-Identifier: BSD-3-Clause
//
// A/B test: Gradients.h and WarpAffine.h vs naive references.
//
//   c++ -std=c++17 -O2 -I modules/math/include test/cpp/test_gradients_warp.cpp \
//       -o /tmp/test_gradients_warp && /tmp/test_gradients_warp
//
// Exit status is the number of failed checks.

#include "Gradients.h"
#include "WarpAffine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace tttrlib;
namespace gr = tttrlib::gradients;
namespace wa = tttrlib::warp_affine;

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

static int cl(int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); }

// Naive clamped 3x3 convolution with explicit taps, in double.
template <class T>
static double conv3(const std::vector<T>& im, int rows, int cols, int r, int c,
                    const double k[3][3]) {
    double acc = 0.0;
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            acc += k[i + 1][j + 1] *
                   static_cast<double>(im[static_cast<size_t>(cl(r + i, rows - 1)) * cols +
                                             cl(c + j, cols - 1)]);
    return acc;
}

template <class T, class S>
static void test_gradients_type(const char* name, Lcg rng, double lo, double hi) {
    std::printf("%s gradients\n", name);
    const int rows = 19, cols = 23;
    std::vector<T> src(static_cast<size_t>(rows) * cols);
    for (T& v : src) v = static_cast<T>(rng.next(lo, hi));

    static const double kdx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
    static const double kdy[3][3] = {{-1, -2, -1}, {0, 0, 0}, {1, 2, 1}};
    static const double klap[3][3] = {{-1, -1, -1}, {-1, 8, -1}, {-1, -1, -1}};

    std::vector<S> dx(src.size()), dy(src.size()), lp(src.size());
    gr::sobel_dx(src.data(), dx.data(), rows, cols);
    gr::sobel_dy(src.data(), dy.data(), rows, cols);
    gr::laplace8(src.data(), lp.data(), rows, cols);
    double wx = 0, wy = 0, wl = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const size_t i = static_cast<size_t>(r) * cols + c;
            const double scale = std::max(1.0, std::abs(static_cast<double>(dx[i])) +
                                                    std::abs(static_cast<double>(src[i])));
            wx = std::max(wx, std::abs(dx[i] - conv3(src, rows, cols, r, c, kdx)) / scale);
            wy = std::max(wy, std::abs(dy[i] - conv3(src, rows, cols, r, c, kdy)) / scale);
            wl = std::max(wl, std::abs(lp[i] - conv3(src, rows, cols, r, c, klap)) / scale);
        }
    report(wx < 1e-12, "sobel_dx matches convolution", wx, 1e-12);
    report(wy < 1e-12, "sobel_dy matches convolution", wy, 1e-12);
    report(wl < 1e-12, "laplace8 matches convolution", wl, 1e-12);

    // constant image: exactly zero derivative everywhere
    std::vector<T> konst(static_cast<size_t>(rows) * cols, static_cast<T>(7));
    std::vector<S> kout(konst.size());
    gr::sobel_dx(konst.data(), kout.data(), rows, cols);
    double kz = 0;
    for (S v : kout) kz = std::max(kz, std::abs(static_cast<double>(v)));
    report(kz == 0.0, "constant image has zero gradient", kz, 0.0);
}

// Naive per-pixel inverse-mapped bilinear sample with replicate border.
static double sample_bilinear(const std::vector<double>& im, int rows, int cols, double sx, double sy) {
    int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
    const double fx = sx - x0, fy = sy - y0;
    if (x0 < -1 || y0 < -1 || x0 > cols || y0 > rows) {  // far outside: nearest
        return im[static_cast<size_t>(cl(y0, rows - 1)) * cols + cl(x0, cols - 1)];
    }
    const int x1 = cl(x0 + 1, cols - 1), y1 = cl(y0 + 1, rows - 1);
    x0 = cl(x0, cols - 1);
    y0 = cl(y0, rows - 1);
    const double top = im[static_cast<size_t>(y0) * cols + x0] * (1 - fx) +
                       im[static_cast<size_t>(y0) * cols + x1] * fx;
    const double bot = im[static_cast<size_t>(y1) * cols + x0] * (1 - fx) +
                       im[static_cast<size_t>(y1) * cols + x1] * fx;
    return top * (1 - fy) + bot * fy;
}

static void test_warp() {
    std::printf("warp_affine\n");
    const int rows = 41, cols = 37;
    Lcg rng(77);
    std::vector<double> src(static_cast<size_t>(rows) * cols);
    for (double& v : src) v = rng.next(0, 100);

    struct Cfg { const char* name; double m[6]; };
    const Cfg cfgs[] = {
        {"identity", {1, 0, 0, 0, 1, 0}},
        {"translate", {1, 0, 3.5, 0, 1, -2.25}},
        {"rotate+scale", {1.2, -0.7, 5.0, 0.7, 1.2, -4.0}},
        {"shear", {1, 0.3, 0, 0.1, 1, 0}},
        // a map that pushes most samples outside: nearest-branch coverage
        {"big translate", {1, 0, 200, 0, 1, 150}},
    };
    for (const Cfg& c : cfgs) {
        std::vector<double> dst(src.size());
        wa::warp_affine(src.data(), dst.data(), rows, cols, c.m);
        // invert the forward map exactly as the kernel does
        const double det = c.m[0] * c.m[4] - c.m[1] * c.m[3];
        const double id = 1.0 / det;
        const double inv[6] = {c.m[4] * id, -c.m[1] * id, (c.m[1] * c.m[5] - c.m[4] * c.m[2]) * id,
                               -c.m[3] * id, c.m[0] * id, (c.m[3] * c.m[2] - c.m[0] * c.m[5]) * id};
        double worst = 0.0, range = 0.0;
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x) {
                const double sx = inv[0] * x + inv[1] * y + inv[2];
                const double sy = inv[3] * x + inv[4] * y + inv[5];
                const double want = sample_bilinear(src, rows, cols, sx, sy);
                const double got = dst[static_cast<size_t>(y) * cols + x];
                range = std::max(range, std::abs(want));
                worst = std::max(worst, std::abs(want - got));
            }
        char what[80];
        std::snprintf(what, sizeof what, "warp %s matches reference", c.name);
        report(worst < 1e-9 * std::max(range, 1.0), what, worst, 1e-9 * std::max(range, 1.0));
    }

    bool threw = false;
    try {
        std::vector<double> dst(src.size());
        const double singular[6] = {1, 2, 0, 2, 4, 0};
        wa::warp_affine(src.data(), dst.data(), rows, cols, singular);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    report(threw, "singular matrix throws", 0.0, 0.0);
}

int main() {
    test_gradients_type<uint8_t, int16_t>("uint8", Lcg(1), 0, 255);
    test_gradients_type<uint16_t, int32_t>("uint16", Lcg(2), 0, 4000);
    test_gradients_type<double, double>("double", Lcg(3), -5, 5);
    test_warp();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
