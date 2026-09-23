// SPDX-License-Identifier: BSD-3-Clause
//
// Image-kernel benchmarks for the ports from ermig1979/Simd's concepts
// (okf/design/simd-port-survey.md): rank filters, integral images, area
// resize, 3-box gaussian, drift estimation. Each kernel is timed against the
// naive alternative a caller would write without it -- direct window sorts
// for the rank filters, direct convolution for the gaussian, per-rectangle
// loops for the integral -- so the printed ratio is what the kernel buys.
//
// Build (from the repository root):
//
//   c++ -std=c++17 -O3 -I modules/math/include \
//       benchmarks/bench_image_kernels.cpp -o /tmp/bench_image_kernels && /tmp/bench_image_kernels

#include "DriftEstimator.h"
#include "Gradients.h"
#include "ImageStat.h"
#include "WarpAffine.h"
#include "FastGaussian.h"
#include "IntegralImage.h"
#include "RankFilters.h"
#include "ResizeImage.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <vector>

using namespace tttrlib;
namespace rf = tttrlib::rank_filters;
namespace ii = tttrlib::integral_image;
namespace rz = tttrlib::resize_image;
namespace fg = tttrlib::fast_gaussian;
namespace de = tttrlib::drift_estimator;
namespace gr = tttrlib::gradients;
namespace is = tttrlib::image_stat;
namespace wa = tttrlib::warp_affine;

/// Thread CPU time, not wall clock (see bench_gradvec.cpp).
static double cpu_ms() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
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

static uint16_t g_img[512 * 512];
static double dst_bicubic[256 * 256];
static double g_imgd[512 * 512];

/// Consumed result checksums -- without these the optimiser deletes the
/// kernels whose outputs nothing reads (measured: max 3x3 "0.00 ms").
static double g_sink = 0.0;

static void fill(uint16_t* im, int rows, int cols, Lcg& rng) {
    for (int i = 0; i < rows * cols; ++i) im[i] = static_cast<uint16_t>(rng.next(0, 4000));
}

/// Naive median: full window gather + sort at every pixel, no edge fast path.
static void median_naive(const uint16_t* src, uint16_t* dst, int rows, int cols) {
    const int offs[25][2] = {{-2,-2},{-2,-1},{-2,0},{-2,1},{-2,2},{-1,-2},{-1,-1},{-1,0},{-1,1},{-1,2},
                             {0,-2},{0,-1},{0,0},{0,1},{0,2},{1,-2},{1,-1},{1,0},{1,1},{1,2},
                             {2,-2},{2,-1},{2,0},{2,1},{2,2}};
    uint16_t w[25];
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            int k = 0;
            for (const auto& o : offs) {
                const int rr = std::min(std::max(r + o[0], 0), rows - 1);
                const int cc = std::min(std::max(c + o[1], 0), cols - 1);
                w[k++] = src[static_cast<std::ptrdiff_t>(rr) * cols + cc];
            }
            std::sort(w, w + 25);
            dst[static_cast<std::ptrdiff_t>(r) * cols + c] = w[12];
        }
}

int main() {
    Lcg rng(7);
    const int N = 512;
    fill(g_img, N, N, rng);
    for (int i = 0; i < N * N; ++i) g_imgd[i] = g_img[i];
    std::printf("512x512 images, thread CPU ms, best of 5\n");

    // ---- rank filters: kernel vs the naive window-sort --------------------
    {
        static uint16_t dst[512 * 512];
        double t0 = 1e30, t1 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            rf::median_filter(g_img, dst, N, N, rf::Shape::Square5x5);
            const double b = cpu_ms();
            median_naive(g_img, dst, N, N);
            const double c = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
        }
        for (int i = 0; i < N * N; ++i) g_sink += dst[i];
        std::printf("median 5x5 uint16   %8.2f  (naive window-sort %8.2f, %5.2fx)\n",
                    t0, t1, t1 / t0);
    }
    {
        static uint16_t dst[512 * 512];
        double t0 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            rf::max_filter(g_img, dst, N, N, rf::Shape::Square3x3);
            t0 = std::min(t0, cpu_ms() - a);
        }
        for (int i = 0; i < N * N; ++i) g_sink += dst[i];
        std::printf("max 3x3 uint16      %8.2f\n", t0);
    }

    // ---- integral: rectangle sums via table vs direct loops ---------------
    {
        std::vector<uint64_t> integral(static_cast<size_t>((N + 1) * (N + 1)));
        double t0 = 1e30, t1 = 1e30;
        const int R = 2000;
        uint64_t sink = 0;
        ii::integral_sum(g_img, N, N, integral.data());
        for (int rep = 0; rep < 5; ++rep) {
            double a = cpu_ms();
            for (int t = 0; t < R; ++t) {
                const int s = t % 100;
                sink += ii::rect_sum(integral.data(), N, s, s, s + 99, s + 99);
            }
            const double b = cpu_ms();
            for (int t = 0; t < R; ++t) {
                const int s = t % 100;
                uint64_t d = 0;
                for (int r = s; r <= s + 99; ++r)
                    for (int c = s; c <= s + 99; ++c)
                        d += g_img[static_cast<std::ptrdiff_t>(r) * N + c];
                sink += d;
            }
            const double c = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
        }
        std::printf("integral 100x100 sums x2000 %8.3f (direct %8.3f, %6.1fx) sink=%llu\n",
                    t0, t1, t1 / t0, static_cast<unsigned long long>(sink & 0xff));
    }

    // ---- area resize: kernel vs explicit overlap-weight loop --------------
    {
        static double dst[256 * 256];
        double t0 = 1e30, t1 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            rz::resize_area(g_imgd, N, N, dst, N / 2, N / 2);
            const double b = cpu_ms();
            // naive: average each 2x2 block (the special-case loop callers write)
            for (int r = 0; r < N / 2; ++r)
                for (int c = 0; c < N / 2; ++c) {
                    double s = 0;
                    for (int i = 0; i < 2; ++i)
                        for (int j = 0; j < 2; ++j)
                            s += g_imgd[static_cast<std::ptrdiff_t>(2 * r + i) * N + 2 * c + j];
                    dst[static_cast<std::ptrdiff_t>(r) * (N / 2) + c] = s / 4;
                }
            const double c = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
        }
        g_sink += dst[0];
        std::printf("area 512->256 double %7.2f (2x2 mean loop %8.2f, %5.2fx, general vs fixed 2x)\n",
                    t0, t1, t1 / t0);
    }

    // ---- gaussian: 3-box vs direct O(k) separable convolution -------------
    {
        static double dst[512 * 512];
        double t0 = 1e30, t1 = 1e30;
        const double sigma = 4.0;
        const int rad = static_cast<int>(4 * sigma + 0.5);
        std::vector<double> w(2 * rad + 1);
        double wsum = 0;
        for (int i = -rad; i <= rad; ++i) {
            w[static_cast<size_t>(i + rad)] = std::exp(-0.5 * i * i / (sigma * sigma));
            wsum += w[static_cast<size_t>(i + rad)];
        }
        for (double& v : w) v /= wsum;
        std::vector<double> tmp(static_cast<size_t>(N) * N), out(tmp.size());
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            fg::gaussian_blur_fast(g_imgd, dst, N, N, sigma);
            const double b = cpu_ms();
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    double acc = 0;
                    for (int i = -rad; i <= rad; ++i) {
                        const int cc = std::min(std::max(c + i, 0), N - 1);
                        acc += w[static_cast<size_t>(i + rad)] * g_imgd[static_cast<std::ptrdiff_t>(r) * N + cc];
                    }
                    tmp[static_cast<std::ptrdiff_t>(r) * N + c] = acc;
                }
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    double acc = 0;
                    for (int i = -rad; i <= rad; ++i) {
                        const int rr = std::min(std::max(r + i, 0), N - 1);
                        acc += w[static_cast<size_t>(i + rad)] * tmp[static_cast<std::ptrdiff_t>(rr) * N + c];
                    }
                    out[static_cast<std::ptrdiff_t>(r) * N + c] = acc;
                }
            const double c = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
        }
        g_sink += dst[0] + out[0];
        std::printf("gaussian sigma=4    %8.2f (direct conv %8.2f, %5.1fx)\n", t0, t1, t1 / t0);
    }

    // ---- drift estimator: absolute cost per estimate -----------------------
    {
        const int rows = 192, cols = 160;
        std::vector<double> ref(static_cast<size_t>(rows) * cols), img(ref.size());
        for (double& v : ref) v = rng.next(0, 1);
        std::vector<double> sm(ref.size());
        fg::gaussian_blur_fast(ref.data(), sm.data(), rows, cols, 2.5);
        // img = sm translated by (3.5, -2.5), bilinear
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const double sr = r + 2.5, sc = c - 3.5;
                const int y0 = std::min(std::max((int)std::floor(sr), 0), rows - 1);
                const int y1 = std::min(y0 + 1, rows - 1);
                const int x0 = std::min(std::max((int)std::floor(sc), 0), cols - 1);
                const int x1 = std::min(x0 + 1, cols - 1);
                const double ay = std::min(std::max(sr - y0, 0.0), 1.0);
                const double ax = std::min(std::max(sc - x0, 0.0), 1.0);
                img[static_cast<std::ptrdiff_t>(r) * cols + c] =
                    sm[static_cast<std::ptrdiff_t>(y0) * cols + x0] * (1 - ax) * (1 - ay) +
                    sm[static_cast<std::ptrdiff_t>(y0) * cols + x1] * ax * (1 - ay) +
                    sm[static_cast<std::ptrdiff_t>(y1) * cols + x0] * (1 - ax) * ay +
                    sm[static_cast<std::ptrdiff_t>(y1) * cols + x1] * ax * ay;
            }
        double t0 = 1e30;
        double dx, dy, score;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            de::estimate_shift(sm.data(), img.data(), rows, cols, 8, dx, dy, score);
            t0 = std::min(t0, cpu_ms() - a);
        }
        g_sink += dx + dy + score;
        std::printf("drift 192x160       %8.3f per estimate (dx=%.2f dy=%.2f score=%.4f)\n",
                    t0, dx, dy, score);
    }

    // ---- gradients: kernel vs a naive clamped convolution ------------------
    {
        std::vector<double> dx(static_cast<size_t>(N) * N);
        static const double kdx[3][3] = {{-1, 0, 1}, {-2, 0, 2}, {-1, 0, 1}};
        double t0 = 1e30, t1 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            gr::sobel_dx(g_imgd, dx.data(), N, N);
            const double b = cpu_ms();
            for (int r = 0; r < N; ++r)
                for (int c = 0; c < N; ++c) {
                    double acc = 0;
                    for (int i = -1; i <= 1; ++i)
                        for (int j = -1; j <= 1; ++j)
                            acc += kdx[i + 1][j + 1] * g_imgd[static_cast<size_t>(std::min(std::max(r + i, 0), N - 1)) * N + std::min(std::max(c + j, 0), N - 1)];
                    dx[static_cast<size_t>(r) * N + c] = acc;
                }
            const double c = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
        }
        g_sink += dx[0];
        std::printf("sobel_dx 512x512      %8.2f (naive convolution %8.2f, %5.2fx)\n",
                    t0, t1, t1 / t0);
    }

    // ---- moments + histogram: absolute cost --------------------------------
    {
        std::vector<double> hist;
        double t0 = 1e30, t1 = 1e30, t2 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            double a = cpu_ms();
            const auto m = is::image_moments(g_imgd, N, N);
            g_sink += m.m00 + m.m10 + m.m01;
            double b = cpu_ms();
            is::value_histogram(g_imgd, N, N, 0, 4000, hist);
            double c = cpu_ms();
            rz::resize_bicubic(g_imgd, N, N, dst_bicubic, N / 2, N / 2);
            double d = cpu_ms();
            t0 = std::min(t0, b - a);
            t1 = std::min(t1, c - b);
            t2 = std::min(t2, d - c);
        }
        g_sink += hist[0];
        std::printf("moments 512x512      %8.3f   histogram %8.3f   bicubic 512->256 %8.2f\n",
                    t0, t1, t2);
    }

    // ---- warp affine: absolute cost ----------------------------------------
    {
        static double dst_w[512 * 512];
        const double mat[6] = {1.0005, -0.001, 2.3, 0.001, 0.9995, -1.7};
        double t0 = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            const double a = cpu_ms();
            wa::warp_affine(g_imgd, dst_w, N, N, mat);
            t0 = std::min(t0, cpu_ms() - a);
        }
        g_sink += dst_w[0];
        std::printf("warp_affine 512x512  %8.2f (rotation+scale+translate)\n", t0);
    }

    std::printf("(sink %.3f -- suppresses dead-code elimination)\n", g_sink);
    return 0;
}
