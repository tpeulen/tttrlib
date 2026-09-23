// SPDX-License-Identifier: BSD-3-Clause
//
// A/B test: RankFilters.h vs brute-force window statistics.
//
//   c++ -std=c++17 -O2 -I modules/math/include test/cpp/test_rank_filters.cpp \
//       -o /tmp/test_rank_filters && /tmp/test_rank_filters
//
// Exit status is the number of failed checks.

#include "RankFilters.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace tttrlib;
namespace rf = tttrlib::rank_filters;

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

// The independent reference: gather the window with replicated edges, sort,
// answer from the sorted vector. Written differently from the header on
// purpose (centre-loop over taps vs header's gather helper).
template <class T>
static std::vector<T> brute_force(const std::vector<T>& src, int rows, int cols,
                                  rf::Shape shape,
                                  int stat /*0 median, 1 min, 2 max, 3 midpoint*/) {
    struct Off { int dr, dc; };
    std::vector<Off> offs;
    switch (shape) {
        case rf::Shape::Rhomb3x3:  offs = {{-1, 0}, {0, -1}, {0, 0}, {0, 1}, {1, 0}}; break;
        case rf::Shape::Square3x3: for (int i = -1; i <= 1; ++i) for (int j = -1; j <= 1; ++j) offs.push_back({i, j}); break;
        case rf::Shape::Square5x5: for (int i = -2; i <= 2; ++i) for (int j = -2; j <= 2; ++j) offs.push_back({i, j}); break;
    }
    std::vector<T> out(static_cast<size_t>(rows) * cols);
    std::vector<T> w(offs.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            for (size_t k = 0; k < offs.size(); ++k) {
                const int rr = std::min(std::max(r + offs[k].dr, 0), rows - 1);
                const int cc = std::min(std::max(c + offs[k].dc, 0), cols - 1);
                w[k] = src[static_cast<size_t>(rr) * cols + cc];
            }
            std::sort(w.begin(), w.end());
            T v{};
            switch (stat) {
                case 0: v = w[w.size() / 2]; break;
                case 1: v = w.front(); break;
                case 2: v = w.back(); break;
                case 3: v = static_cast<T>((w.front() + w.back()) / 2); break;
            }
            out[static_cast<size_t>(r) * cols + c] = v;
        }
    }
    return out;
}

template <class T>
static void test_type(const char* name, Lcg rng, double lo, double hi, bool quantize) {
    const int rows = 23, cols = 17;
    std::vector<T> src(static_cast<size_t>(rows) * cols);
    for (size_t i = 0; i < src.size(); ++i) {
        double v = rng.next(lo, hi);
        if (quantize) v = std::floor(v * 8.0);  // heavy ties: exercise equal keys
        src[i] = static_cast<T>(v);
    }
    for (rf::Shape shape : {rf::Shape::Rhomb3x3, rf::Shape::Square3x3, rf::Shape::Square5x5}) {
        const char* sh = shape == rf::Shape::Rhomb3x3 ? "rhomb3" :
                         shape == rf::Shape::Square3x3 ? "square3" : "square5";
        std::vector<T> got(src.size());
        std::vector<T> want;
        const char* st;
        for (int stat = 0; stat <= 3; ++stat) {
            switch (stat) {
                case 0: rf::median_filter(src.data(), got.data(), rows, cols, shape); st = "median"; break;
                case 1: rf::min_filter(src.data(), got.data(), rows, cols, shape); st = "min"; break;
                case 2: rf::max_filter(src.data(), got.data(), rows, cols, shape); st = "max"; break;
                case 3: rf::midpoint_filter(src.data(), got.data(), rows, cols, shape); st = "midpoint"; break;
            }
            want = brute_force(src, rows, cols, shape, stat);
            const bool ok = got == want;
            if (!ok)
                report(false, (std::string(name) + " " + st + " " + sh + " matches brute force").c_str(),
                       1.0, 0.0);
            else if (stat == 0)
                report(true, (std::string(name) + " " + st + " " + sh + " matches brute force").c_str(),
                       0.0, 0.0);
        }
    }
    // constant image is a fixed point of every filter
    std::vector<T> konst(static_cast<size_t>(rows) * cols, T{7});
    std::vector<T> got(static_cast<size_t>(rows) * cols);
    rf::median_filter(konst.data(), got.data(), rows, cols, rf::Shape::Square5x5);
    report(std::all_of(got.begin(), got.end(), [](T v) { return v == T{7}; }),
           (std::string(name) + ": constant image invariant under median").c_str(), 0.0, 0.0);
}

static void test_empty_throws() {
    std::vector<double> a(4), b(4);
    bool threw = false;
    try {
        rf::median_filter(a.data(), b.data(), 0, 4, rf::Shape::Square3x3);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    report(threw, "empty image throws", 0.0, 0.0);
}

// >= 128 rows takes the striped OpenMP path inside median_square_hist; the
// small images above never reach it. Brute force on a subsample of pixels
// keeps the check fast while still crossing stripe boundaries.
static void test_median_striped_path_matches_brute_force() {
    std::printf("striped median path\n");
    const int rows = 256, cols = 96;
    Lcg rng(99);
    std::vector<uint16_t> src(static_cast<size_t>(rows) * cols);
    for (uint16_t& v : src) v = static_cast<uint16_t>(rng.next(0, 500));
    std::vector<uint16_t> got(src.size());
    rf::median_filter(src.data(), got.data(), rows, cols, rf::Shape::Square5x5);
    int checked = 0;
    double bad = 0;
    for (int r = 0; r < rows; r += 7)
        for (int c = 0; c < cols; c += 5) {
            uint16_t w[25];
            int k = 0;
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j) {
                    const int rr = std::min(std::max(r + i, 0), rows - 1);
                    const int cc = std::min(std::max(c + j, 0), cols - 1);
                    w[k++] = src[static_cast<size_t>(rr) * cols + cc];
                }
            std::sort(w, w + 25);
            if (got[static_cast<size_t>(r) * cols + c] != w[12]) bad += 1;
            ++checked;
        }
    report(bad == 0, "striped median exact on 700+ pixels incl. stripe boundaries",
           bad, 0.0);
    (void)checked;
}

int main() {
    test_type<uint8_t>("uint8", Lcg(1), 0, 255, true);
    test_type<uint16_t>("uint16", Lcg(2), 0, 4000, true);
    test_type<float>("float", Lcg(3), -5, 5, false);
    test_type<double>("double", Lcg(4), -5, 5, false);
    test_empty_throws();
    test_median_striped_path_matches_brute_force();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
