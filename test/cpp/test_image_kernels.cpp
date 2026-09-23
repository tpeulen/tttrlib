// SPDX-License-Identifier: BSD-3-Clause
//
// A/B test: IntegralImage.h, ResizeImage.h, FastGaussian.h vs direct
// reference computations.
//
//   c++ -std=c++17 -O2 -I modules/math/include test/cpp/test_image_kernels.cpp \
//       -o /tmp/test_image_kernels && /tmp/test_image_kernels
//
// Exit status is the number of failed checks.

#include "FastGaussian.h"
#include "ImageStat.h"
#include "IntegralImage.h"
#include "ResizeImage.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace tttrlib;
namespace ii = tttrlib::integral_image;
namespace rz = tttrlib::resize_image;
namespace is = tttrlib::image_stat;
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

static void test_integral() {
    std::printf("integral images\n");
    const int rows = 19, cols = 23;
    std::vector<uint16_t> src(static_cast<size_t>(rows) * cols);
    Lcg rng(11);
    for (uint16_t& v : src) v = static_cast<uint16_t>(rng.next(0, 1000));
    std::vector<uint64_t> integral(static_cast<size_t>((rows + 1) * (cols + 1)));
    ii::integral_sum(src.data(), rows, cols, integral.data());
    // table corners vs a direct double loop
    double worst = 0.0;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            uint64_t s = 0;
            for (int i = 0; i < r; ++i)
                for (int j = 0; j < c; ++j)
                    s += src[static_cast<size_t>(i) * cols + j];
            worst = std::max(worst, std::abs(static_cast<double>(s - integral[static_cast<size_t>(r) * (cols + 1) + c])));
        }
    report(worst == 0.0, "uint64 sum table exact", worst, 0.0);

    // rectangle sums vs direct window sums
    double rect_worst = 0.0;
    for (int t = 0; t < 50; ++t) {
        const int r0 = static_cast<int>(rng.next(0, rows - 1));
        const int r1 = static_cast<int>(rng.next(r0, rows - 1));
        const int c0 = static_cast<int>(rng.next(0, cols - 1));
        const int c1 = static_cast<int>(rng.next(c0, cols - 1));
        uint64_t direct = 0;
        for (int i = r0; i <= r1; ++i)
            for (int j = c0; j <= c1; ++j)
                direct += src[static_cast<size_t>(i) * cols + j];
        rect_worst = std::max(rect_worst,
                              std::abs(static_cast<double>(direct - ii::rect_sum(integral.data(), cols, r0, c0, r1, c1))));
    }
    report(rect_worst == 0.0, "rect_sum exact on 50 random rectangles", rect_worst, 0.0);

    // sum of squares in the same pass, double accumulator
    std::vector<uint64_t> sum2(static_cast<size_t>((rows + 1) * (cols + 1)));
    std::vector<double> sq2(static_cast<size_t>((rows + 1) * (cols + 1)));
    ii::integral_sum_sqsum(src.data(), rows, cols, sum2.data(), sq2.data());
    double sq_worst = 0.0;
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) {
            uint64_t s = 0;
            for (int i = 0; i < r; ++i)
                for (int j = 0; j < c; ++j) {
                    const double v = src[static_cast<size_t>(i) * cols + j];
                    s += static_cast<uint64_t>(v * v);
                }
            sq_worst = std::max(sq_worst, std::abs(static_cast<double>(s) - sq2[static_cast<size_t>(r) * (cols + 1) + c]));
        }
    report(sq_worst < 1e-6, "sum-of-squares table matches", sq_worst, 1e-6);
}

// Independent area-resize reference: overlap weights computed from scratch.
static std::vector<double> area_reference(const std::vector<double>& src, int rows, int cols,
                                          int drows, int dcols) {
    std::vector<double> out(static_cast<size_t>(drows) * dcols);
    for (int dr = 0; dr < drows; ++dr)
        for (int dc = 0; dc < dcols; ++dc) {
            const double lo_r = static_cast<double>(dr) * rows / drows;
            const double hi_r = static_cast<double>(dr + 1) * rows / drows;
            const double lo_c = static_cast<double>(dc) * cols / dcols;
            const double hi_c = static_cast<double>(dc + 1) * cols / dcols;
            double acc = 0, wsum = 0;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) {
                    const double w = std::max(0.0, std::min(hi_r, r + 1.0) - std::max(lo_r, static_cast<double>(r))) *
                                     std::max(0.0, std::min(hi_c, c + 1.0) - std::max(lo_c, static_cast<double>(c)));
                    acc += w * src[static_cast<size_t>(r) * cols + c];
                    wsum += w;
                }
            out[static_cast<size_t>(dr) * dcols + dc] = acc / wsum;
        }
    return out;
}

static void test_resize() {
    std::printf("resize\n");
    const int rows = 16, cols = 12;
    std::vector<double> src(static_cast<size_t>(rows) * cols);
    Lcg rng(21);
    for (double& v : src) v = rng.next(0, 100);

    struct Scale { int r, c; };
    for (const Scale& s : {Scale{8, 6}, Scale{32, 24}, Scale{33, 17}, Scale{5, 11}}) {
        std::vector<double> got(static_cast<size_t>(s.r) * s.c);
        rz::resize_area(src.data(), rows, cols, got.data(), s.r, s.c);
        const std::vector<double> want = area_reference(src, rows, cols, s.r, s.c);
        double worst = 0.0;
        for (size_t i = 0; i < got.size(); ++i)
            worst = std::max(worst, std::abs(got[i] - want[i]));
        report(worst < 1e-9, "area resize matches overlap-weight reference", worst, 1e-9);
    }

    // bilinear vs the formula evaluated per pixel from scratch
    const int dr = 29, dc = 19;
    std::vector<double> got(static_cast<size_t>(dr) * dc);
    rz::resize_bilinear(src.data(), rows, cols, got.data(), dr, dc);
    double worst = 0.0;
    for (int r = 0; r < dr; ++r)
        for (int c = 0; c < dc; ++c) {
            const double sy = (r + 0.5) * rows / dr - 0.5;
            const double sx = (c + 0.5) * cols / dc - 0.5;
            const int y0 = std::min(std::max(static_cast<int>(std::floor(sy)), 0), rows - 1);
            const int y1 = std::min(y0 + 1, rows - 1);
            const int x0 = std::min(std::max(static_cast<int>(std::floor(sx)), 0), cols - 1);
            const int x1 = std::min(x0 + 1, cols - 1);
            const double ay = std::min(std::max(sy - y0, 0.0), 1.0);
            const double ax = std::min(std::max(sx - x0, 0.0), 1.0);
            const double top = src[static_cast<size_t>(y0) * cols + x0] * (1 - ax) + src[static_cast<size_t>(y0) * cols + x1] * ax;
            const double bot = src[static_cast<size_t>(y1) * cols + x0] * (1 - ax) + src[static_cast<size_t>(y1) * cols + x1] * ax;
            const double want = top * (1 - ay) + bot * ay;
            worst = std::max(worst, std::abs(want - got[static_cast<size_t>(r) * dc + c]));
        }
    report(worst < 1e-9, "bilinear matches per-pixel formula", worst, 1e-9);
}

static double gauss_kernel(double x, double sigma) {
    return std::exp(-0.5 * x * x / (sigma * sigma));
}

// Direct O(k) separable gaussian with edge replication -- the reference.
static std::vector<double> gaussian_reference(const std::vector<double>& src, int rows, int cols, double sigma) {
    const int rad = static_cast<int>(4.0 * sigma + 0.5);
    std::vector<double> w(2 * rad + 1);
    double wsum = 0;
    for (int i = -rad; i <= rad; ++i) {
        w[static_cast<size_t>(i + rad)] = gauss_kernel(i, sigma);
        wsum += w[static_cast<size_t>(i + rad)];
    }
    for (double& v : w) v /= wsum;
    std::vector<double> tmp(static_cast<size_t>(rows) * cols), out(tmp.size());
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            double acc = 0;
            for (int i = -rad; i <= rad; ++i) {
                const int cc = std::min(std::max(c + i, 0), cols - 1);
                acc += w[static_cast<size_t>(i + rad)] * src[static_cast<size_t>(r) * cols + cc];
            }
            tmp[static_cast<size_t>(r) * cols + c] = acc;
        }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            double acc = 0;
            for (int i = -rad; i <= rad; ++i) {
                const int rr = std::min(std::max(r + i, 0), rows - 1);
                acc += w[static_cast<size_t>(i + rad)] * tmp[static_cast<size_t>(rr) * cols + c];
            }
            out[static_cast<size_t>(r) * cols + c] = acc;
        }
    return out;
}

static void test_fast_gaussian() {
    std::printf("fast gaussian\n");
    const int rows = 48, cols = 40;
    std::vector<double> noise(static_cast<size_t>(rows) * cols);
    Lcg rng(31);
    for (double& v : noise) v = rng.next(0, 100);
    // band-limited input: on white noise the two kernels' L2 norms (not their
    // shapes) differ by ~20% and the comparison is meaningless; real images
    // are band-limited
    const std::vector<double> src = gaussian_reference(noise, rows, cols, 1.0);
    std::vector<double> got(src.size());
    for (double sigma : {0.8, 2.0, 6.0}) {
        // sigma=6: the box widths are quantised to odd integers, so the shape
        // error grows with sigma -- the honest bound is 10% there, 5% below
        const double bound = (sigma < 3.0) ? 0.05 : 0.10;
        fg::gaussian_blur_fast(src.data(), got.data(), rows, cols, sigma);
        const std::vector<double> want = gaussian_reference(src, rows, cols, sigma);
        double range = 0.0;
        for (double v : want) range = std::max(range, std::abs(v));
        double worst = 0.0;
        for (size_t i = 0; i < got.size(); ++i)
            worst = std::max(worst, std::abs(got[i] - want[i]));
        char what[64];
        std::snprintf(what, sizeof what, "3-box within %s of direct gaussian (sigma=%.1f)",
                      sigma < 3.0 ? "5%" : "10%", sigma);
        report(worst <= bound * range, what, worst, bound * range);
    }
    // constant image is a fixed point
    std::vector<double> konst(src.size(), 42.0), kout(src.size());
    fg::gaussian_blur_fast(konst.data(), kout.data(), rows, cols, 3.0);
    double worst = 0.0;
    for (double v : kout) worst = std::max(worst, std::abs(v - 42.0));
    report(worst < 1e-12, "constant image invariant", worst, 1e-12);
    // sigma below resolution: identity
    std::vector<double> idout(src.size());
    fg::gaussian_blur_fast(src.data(), idout.data(), rows, cols, 0.1);
    double ident = 0.0;
    for (size_t i = 0; i < src.size(); ++i) ident = std::max(ident, std::abs(src[i] - idout[i]));
    report(ident == 0.0, "sigma < 0.3 is the identity", ident, 0.0);
}

// Bicubic vs a manual per-pixel cubic evaluation (same Keys a=-0.5 weights,
// clamped taps, pixel-centre positioning) written independently.
static void test_bicubic() {
    std::printf("bicubic\n");
    const int rows = 12, cols = 9;
    std::vector<double> src(static_cast<size_t>(rows) * cols);
    Lcg rng(41);
    for (double& v : src) v = rng.next(0, 100);
    for (auto [dr, dc] : {std::pair<int, int>{23, 19}, {7, 30}, {12, 9}}) {
        std::vector<double> got(static_cast<size_t>(dr) * dc);
        rz::resize_bicubic(src.data(), rows, cols, got.data(), dr, dc);
        auto cw = [](double d, double w[4]) {
            const double d2 = d * d, d3 = d2 * d;
            w[0] = -0.5 * d3 + d2 - 0.5 * d;
            w[1] = 1.5 * d3 - 2.5 * d2 + 1.0;
            w[2] = -1.5 * d3 + 2.0 * d2 + 0.5 * d;
            w[3] = 0.5 * d3 - 0.5 * d2;
        };
        auto axis = [](int ssz, int dsz, int i, int taps[4]) {
            const double pos = (i + 0.5) * ssz / dsz - 0.5;
            int idx = static_cast<int>(std::floor(pos));
            double d = pos - idx;
            if (idx <= 0) { idx = 0; d = 0.0; }
            if (idx >= ssz - 1) { idx = ssz - 1; d = 0.0; }
            for (int k = 0; k < 4; ++k) {
                int t = idx - 1 + k;
                taps[k] = t < 0 ? 0 : (t > ssz - 1 ? ssz - 1 : t);
            }
            return d;
        };
        double worst = 0.0;
        for (int r = 0; r < dr; ++r) {
            int rt[4];
            const double rd = axis(rows, dr, r, rt);
            double wr[4];
            cw(rd, wr);
            for (int c = 0; c < dc; ++c) {
                int ct[4];
                const double cd = axis(cols, dc, c, ct);
                double wc[4];
                cw(cd, wc);
                double want = 0.0;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        want += wr[i] * wc[j] * src[static_cast<size_t>(rt[i]) * cols + ct[j]];
                worst = std::max(worst, std::abs(want - got[static_cast<size_t>(r) * dc + c]));
            }
        }
        report(worst < 1e-9, "bicubic matches manual cubic evaluation", worst, 1e-9);
    }
}

// Histogram counts vs a tally map; moments vs direct loops; centroid of a
// synthetic Gaussian spot within 0.01 px of its true centre.
static void test_histogram_moments() {
    std::printf("histogram and moments\n");
    const int rows = 31, cols = 29;
    Lcg rng(43);
    std::vector<uint16_t> src(static_cast<size_t>(rows) * cols);
    for (uint16_t& v : src) v = static_cast<uint16_t>(rng.next(0, 99));
    std::vector<double> hist;
    is::value_histogram(src.data(), rows, cols, 0, 99, hist);
    std::vector<double> tally(100, 0.0);
    for (uint16_t v : src) tally[v] += 1.0;
    double worst = 0.0;
    for (int b = 0; b < 100; ++b) worst = std::max(worst, std::abs(hist[b] - tally[b]));
    report(worst == 0.0, "uint16 histogram exact", worst, 0.0);

    // masked: every other pixel skipped
    std::vector<uint8_t> mask(static_cast<size_t>(rows) * cols);
    double want_count = 0.0;
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = (i % 2) ? 1 : 0;
        want_count += mask[i];
    }
    is::value_histogram(src.data(), rows, cols, 0, 99, hist, mask.data());
    double masked_count = 0.0;
    for (double v : hist) masked_count += v;
    report(std::abs(masked_count - want_count) < 1e-9,
           "masked histogram counts the unmasked pixels", masked_count, want_count);

    // moments vs direct double loops
    std::vector<double> ds(src.size());
    for (size_t i = 0; i < src.size(); ++i) ds[i] = src[i];
    const is::Moments m = is::image_moments(ds.data(), rows, cols);
    double ref[6] = {0, 0, 0, 0, 0, 0};
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const double v = ds[static_cast<size_t>(r) * cols + c];
            ref[0] += v;
            ref[1] += v * c;
            ref[2] += v * r;
            ref[3] += v * r * c;
            ref[4] += v * c * c;
            ref[5] += v * r * r;
        }
    double wm = std::max({std::abs(m.m00 - ref[0]), std::abs(m.m10 - ref[1]),
                          std::abs(m.m01 - ref[2]), std::abs(m.m11 - ref[3]),
                          std::abs(m.m20 - ref[4]), std::abs(m.m02 - ref[5])});
    report(wm < 1e-6, "moments match direct loops", wm, 1e-6);

    // Gaussian spot centroid
    std::vector<double> spot(static_cast<size_t>(rows) * cols, 0.0);
    const double cx = 14.3, cy = 17.6, sig = 2.5;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            spot[static_cast<size_t>(r) * cols + c] =
                1000.0 * std::exp(-((c - cx) * (c - cx) + (r - cy) * (r - cy)) / (2 * sig * sig));
    const is::Moments sm = is::image_moments(spot.data(), rows, cols);
    report(std::hypot(sm.centroid_x() - cx, sm.centroid_y() - cy) < 0.01,
           "Gaussian centroid within 0.01 px",
           std::hypot(sm.centroid_x() - cx, sm.centroid_y() - cy), 0.01);
}

int main() {
    test_integral();
    test_resize();
    test_fast_gaussian();
    test_bicubic();
    test_histogram_moments();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
