// SPDX-License-Identifier: BSD-3-Clause
//
// BurstPipeline against the unfused path, on a simulated two-detector stream.
//
// The reference is built the way ChiSurf builds it, out of the library's own
// pieces: the search on a subset TTTR, the burst span marked in a full-length mask,
// the delta-T filter, the gap fill and BurstFilter::bursts_from_mask, then brute
// force per-burst features and histograms. The pipeline must give the same bursts,
// the same features bit for bit and the same decays, at a chunk size small enough
// that every burst search crosses chunk boundaries many times.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "BurstFilter.h"
#include "BurstPipeline.h"
#include "TTTR.h"
#include "TTTRHeader.h"

using namespace tttrlib;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

struct Sim {
    std::vector<unsigned long long> macro;
    std::vector<unsigned short> micro;
    std::vector<signed char> rout;
};

/*! Poisson background plus bursts; channels 0..3, micro 0..499 (two PIE windows). */
static Sim simulate(unsigned seed, int n_bursts) {
    std::mt19937_64 rng(seed);
    std::exponential_distribution<double> bg(1.0 / 8000.0);     // ticks
    std::exponential_distribution<double> fast(1.0 / 60.0);
    std::uniform_int_distribution<int> ch(0, 3);
    std::exponential_distribution<double> life(1.0 / 40.0);
    Sim s;
    double t = 0;
    for (int b = 0; b <= n_bursts; ++b) {
        const double t_end = t + 2e5;
        while (t < t_end) {
            t += bg(rng);
            s.macro.push_back(static_cast<unsigned long long>(t));
            s.rout.push_back(static_cast<signed char>(ch(rng)));
            s.micro.push_back(static_cast<unsigned short>(rng() % 500));
        }
        if (b == n_bursts) break;
        const int n = 30 + static_cast<int>(rng() % 200);
        for (int k = 0; k < n; ++k) {
            t += fast(rng);
            s.macro.push_back(static_cast<unsigned long long>(t));
            const int c = ch(rng);
            s.rout.push_back(static_cast<signed char>(c));
            const int base = (c >= 2 && (rng() % 3 == 0)) ? 250 : 10;
            s.micro.push_back(static_cast<unsigned short>(std::min(499.0, base + life(rng))));
        }
    }
    return s;
}

static std::shared_ptr<TTTR> make_tttr(Sim &s) {
    auto t = std::make_shared<TTTR>();
    std::vector<signed char> et(s.macro.size(), 0);
    t->append_events(s.macro.data(), static_cast<int>(s.macro.size()), s.micro.data(),
                     static_cast<int>(s.micro.size()), s.rout.data(), static_cast<int>(s.rout.size()),
                     et.data(), static_cast<int>(et.size()), false, 0);
    if (t->get_header()) {
        t->get_header()->set_macro_time_resolution(40e-9);
        t->get_header()->set_micro_time_resolution(80e-12);
        t->get_header()->set_number_of_micro_time_channels(500);
    }
    return t;
}

struct Det { std::vector<int> chs; int lo, hi; };

static bool member(const Det &d, int c, int m) {
    bool ch = false;
    for (int x : d.chs) ch |= (x == c);
    return ch && m >= d.lo && m < d.hi;
}

/*! The unfused selection: search on the subset, mask, delta-T, gap fill, bursts_from_mask. */
static std::vector<std::pair<long long, long long>> reference_bursts(
        TTTR &t, const Sim &s, const std::vector<Det> &dets, const std::string &algo,
        int max_gap, int min_photons, double dt_max_ms) {
    const int N = static_cast<int>(s.macro.size());
    std::vector<int> idx;
    for (int i = 0; i < N; ++i) {
        bool in = false;
        for (const auto &d : dets) in |= member(d, s.rout[i], s.micro[i]);
        if (in) idx.push_back(i);
    }
    TTTR sub(t, idx.data(), static_cast<int>(idx.size()));
    std::vector<long long> found = algo == "cusum_sprt"
        ? sub.burst_search_cusum_sprt(5, 3000.0, 4.0, 0.05, 0.05)
        : sub.burst_search_sliding_window(10, 8, 20e-6);
    std::vector<unsigned char> mask(N, 0);
    for (size_t k = 0; k + 1 < found.size(); k += 2)
        for (int i = idx[found[k]]; i <= idx[found[k + 1]]; ++i) mask[i] = 1;
    for (int i = 0; i < N; ++i) {
        const double d = i == 0 ? 0.0 : static_cast<double>(s.macro[i] - s.macro[i - 1]) * 40e-9 * 1000.0;
        if (!(d <= dt_max_ms)) mask[i] = 0;
    }
    // fill_small_gaps_in_array: zero runs of length <= max_gap + 1 between ones
    if (max_gap > 0) {
        std::vector<int> starts, stops;
        for (int i = 0; i <= N; ++i) {
            const int a = i < N ? mask[i] : 0, b = i > 0 ? mask[i - 1] : 0;
            if (a && !b) starts.push_back(i);
            if (!a && b) stops.push_back(i);
        }
        for (size_t k = 0; k + 1 < starts.size(); ++k)
            if (starts[k + 1] - stops[k] - 1 <= max_gap)
                for (int i = stops[k]; i < starts[k + 1]; ++i) mask[i] = 1;
    }
    long long *out = nullptr; int d1 = 0, d2 = 0;
    BurstFilter::bursts_from_mask(mask.data(), N, max_gap, &out, &d1, &d2);
    std::vector<std::pair<long long, long long>> b;
    for (int k = 0; k < d1; ++k)
        if (out[2 * k + 1] - out[2 * k] + 1 >= std::max(2, min_photons)) b.emplace_back(out[2 * k], out[2 * k + 1]);
    std::free(out);
    return b;
}

static std::string config(const std::string &algo, int chunk, int max_gap, int min_photons) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf), R"({
      "detectors": [{"name": "G", "chs": [0, 1], "gates": [[0, 250]]},
                    {"name": "R", "chs": [2, 3], "gates": [[0, 250]]},
                    {"name": "RR", "chs": [2, 3], "gates": [[250, 500]]}],
      "windows": [{"name": "prompt", "lo": 10, "hi": 250}, {"name": "delayed", "lo": 250, "hi": 500}],
      "search": {"algorithm": "%s", "scope": "detectors",
                 "parameters": {"min_photons": 5, "background_cps": 3000.0,
                                "signal_to_background_ratio": 4.0, "alpha": 0.05, "beta": 0.05,
                                "L": 10, "m": 8, "T": 2e-05}},
      "selection": {"max_gap": %d, "min_photons": %d, "dt_max_active": true, "dt_max_ms": 0.15},
      "decays": {"binning": 2, "n_bins": 250},
      "confidence": false,
      "micro_time_resolution_ns": 0.08,
      "macro_time_resolution": 4e-08,
      "chunk_size": %d
    })", algo.c_str(), max_gap, min_photons, chunk);
    return buf;
}

static void check_algorithm(const std::string &algo) {
    Sim s = simulate(algo == "cusum_sprt" ? 7 : 11, 300);
    auto t = make_tttr(s);
    const std::vector<Det> dets = {{{0, 1}, 0, 250}, {{2, 3}, 0, 250}, {{2, 3}, 250, 500}};
    const int max_gap = 3, min_photons = 20;
    auto ref = reference_bursts(*t, s, dets, algo, max_gap, min_photons, 0.15);
    CHECK(ref.size() > 100, "%s: reference found only %zu bursts", algo.c_str(), ref.size());

    for (int chunk : {1024, 1 << 20}) {
        BurstPipeline pipe(config(algo, chunk, max_gap, min_photons));
        auto res = pipe.run(t);
        CHECK(res->n_bursts() == static_cast<long long>(ref.size()), "%s chunk %d: %lld bursts, reference %zu",
              algo.c_str(), chunk, res->n_bursts(), ref.size());
        long long *first, *last; int n;
        res->index_column("first_photon", &first, &n);
        res->index_column("last_photon", &last, &n);
        const size_t nb = std::min(ref.size(), static_cast<size_t>(n));
        size_t bad = 0;
        for (size_t b = 0; b < nb; ++b) bad += (first[b] != ref[b].first || last[b] != ref[b].second);
        CHECK(bad == 0, "%s chunk %d: %zu bursts differ", algo.c_str(), chunk, bad);

        // features and decays by brute force on the pipeline's bursts
        long long *np_g; double *mm_g, *dur, *micro_rr;
        res->index_column("n_photons|G", &np_g, &n);
        res->column("mean_macro_time_ms|prompt@G", &mm_g, &n);
        res->column("duration_ms", &dur, &n);
        res->column("mean_micro_time_ns|RR", &micro_rr, &n);
        long long *ip; int *bins, *cnt; int ni, nbins, nc;
        const int g_r = res->decay_group_of("R");
        CHECK(g_r == res->decay_group_of("RR"), "R and RR share a decay group");
        res->decay_indptr(g_r, &ip, &ni);
        res->decay_bins(g_r, &bins, &nbins);
        res->decay_counts(g_r, &cnt, &nc);
        size_t bad_f = 0, bad_d = 0;
        for (size_t b = 0; b < nb; ++b) {
            const long long s0 = first[b], e0 = last[b];
            long long ng = 0, f = -1, l = -1, nrr = 0, sum_rr = 0;
            std::vector<long long> h(500, 0);
            for (long long i = s0; i <= e0; ++i) {
                if (member(dets[0], s.rout[i], s.micro[i])) {
                    ++ng;
                    if (s.micro[i] >= 10 && s.micro[i] < 250) { if (f < 0) f = i; l = i; }
                }
                if (member(dets[2], s.rout[i], s.micro[i])) { ++nrr; sum_rr += s.micro[i]; }
                if (s.rout[i] == 2 || s.rout[i] == 3) {
                    const int bin = s.micro[i] / 2;
                    if (bin < 250) h[(s.rout[i] == 3 ? 250 : 0) + bin] += 1;
                }
            }
            const double mm = f < 0 ? -1.0 : (static_cast<double>(s.macro[l] + s.macro[f]) / 2.0) * 4e-8 * 1e3;
            const double d = static_cast<double>(s.macro[e0] - s.macro[s0]) * 4e-8 * 1e3;
            const double mrr = nrr ? static_cast<double>(sum_rr) / static_cast<double>(nrr) * 0.08 : -1.0;
            bad_f += (np_g[b] != ng) + (mm_g[b] != mm) + (dur[b] != d) + (micro_rr[b] != mrr);
            std::vector<long long> got(500, 0);
            for (long long k = ip[b]; k < ip[b + 1]; ++k) got[bins[k]] += cnt[k];
            bad_d += (got != h);
        }
        CHECK(bad_f == 0, "%s chunk %d: %zu feature mismatches", algo.c_str(), chunk, bad_f);
        CHECK(bad_d == 0, "%s chunk %d: %zu decay rows differ", algo.c_str(), chunk, bad_d);
        std::printf("%s chunk %7d: %lld bursts, stats %s\n", algo.c_str(), chunk, res->n_bursts(),
                    res->stats_json().c_str());
    }
}

/*! decays() on given bursts equals the decays run() produced for them. */
static void check_decays_call() {
    Sim s = simulate(3, 100);
    auto t = make_tttr(s);
    BurstPipeline pipe(config("cusum_sprt", 4096, 3, 20));
    auto res = pipe.run(t);
    long long *first, *last; int n;
    res->index_column("first_photon", &first, &n);
    res->index_column("last_photon", &last, &n);
    std::vector<long long> f(first, first + n), l(last, last + n);
    auto again = pipe.decays(t, f.data(), n, l.data(), n);
    for (int g = 0; g < res->n_decay_groups(); ++g) {
        long long *a, *b; int na, nb;
        res->decay_sum(g, &a, &na);
        again->decay_sum(g, &b, &nb);
        bool same = na == nb;
        for (int k = 0; same && k < na; ++k) same = a[k] == b[k];
        CHECK(same, "decays(): group %d summed decay differs from run()", g);
    }
}

int main() {
    check_algorithm("cusum_sprt");
    check_algorithm("sliding_window");
    check_decays_call();
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("all passed\n");
    return 0;
}
