// StreamingBurstAnalyzer.h: live burst analysis agrees with the batch path.
//
// On a synthetic stream (Poisson background with planted bursts, plus photons
// on a channel no detector declares) the analyzer must equal a batch reference
// written out here:
//   - PIE: bursts equal the batch sliding window on the channel-selected stream
//     (index for index) and StreamingBurstDetector; per-detector and per-window
//     counts equal a direct count; ratios equal apparent_es; corrected FRET
//     equals corrected_es with fixed backgrounds;
//   - µs-ALEX: phase windows count photons by (macro mod period);
//   - anisotropy equals DecayFit's (Fp - g Fs) / (Fp (1 - 3 l2) + (2 - 3 l1) g Fs);
//   - a search on a channel subset counts the other detectors' photons that
//     fall within each burst's time span;
//   - histograms hold every in-range burst; burst decays sum to burst counts;
//   - DCBS raises; clear() and replay give the same table.
//
//     c++ -std=c++17 -O2 -I modules/streaming/include \
//         -o t test/cpp/test_streaming_burst_analyzer.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-10-26.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "StreamingBurstDetector.h"
#include "StreamingBurstAnalyzer.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-66s %.6g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

struct Photon { uint64_t t; uint16_t micro; int ch; };

// background + planted bursts; channels 0..3 used, 5 undeclared
static std::vector<Photon> stream(unsigned seed, double dt) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::vector<Photon> ph;
    double t = 0.0;
    for (int b = 0; b < 300; ++b) {
        const double t_end = t + 0.01 / dt;
        while (true) {
            t += 1.0 + std::floor(-std::log(1.0 - U(rng)) * (2e-4 / dt));
            if (t > t_end) break;
            const int ch = U(rng) < 0.1 ? 5 : static_cast<int>(U(rng) * 4);
            ph.push_back({static_cast<uint64_t>(t), static_cast<uint16_t>(U(rng) * 4096), ch});
        }
        const int n = 40 + static_cast<int>(U(rng) * 80);
        const double e = (b % 2) ? 0.3 : 0.8;
        for (int k = 0; k < n; ++k) {
            t += 1.0 + std::floor(-std::log(1.0 - U(rng)) * (5e-6 / dt));
            const bool donor_exc = U(rng) < 0.5;
            const uint16_t mi = static_cast<uint16_t>(donor_exc ? U(rng) * 2048 : 2048 + U(rng) * 2048);
            const bool acc = donor_exc ? U(rng) < e : U(rng) < 0.95;
            const int pol = U(rng) < 0.6 ? 0 : 2;          // 0/1 parallel, 2/3 perpendicular
            ph.push_back({static_cast<uint64_t>(t), mi, (acc ? 1 : 0) + pol});
        }
    }
    return ph;
}

int main() {
    using tttrlib::StreamingBurstAnalyzer;
    using tttrlib::StreamingBurstDetector;
    const double dt = 12.5e-9, micro_dt = 16e-12;
    const int L = 20, m = 10;
    const double T = 1e-4;
    const std::vector<Photon> ph = stream(11, dt);

    // ---- PIE, default search on every detector channel ------------------------
    StreamingBurstAnalyzer a(dt, micro_dt);
    a.add_detector("green", {0, 2}, {});
    a.add_detector("red", {1, 3}, {});
    a.add_window("prompt", 0, 2048);
    a.add_window("delayed", 2048, 4096);
    a.set_search(L, m, T);
    a.add_ratio("Proximity Ratio", {"prompt red"}, {"prompt green", "prompt red"}, false);
    a.add_ratio("Stoichiometry", {"prompt green", "prompt red"},
                {"prompt green", "prompt red", "delayed red"}, false);
    const double g = 1.2, be = 0.8, al = 0.06, de = 0.04;
    a.add_corrected_fret("FRET Efficiency", "Stoichiometry (corrected)",
                         "prompt green", "prompt red", "delayed red", g, be, al, de, true);
    a.set_background_rate("prompt green", 1500.0);
    a.set_background_rate("prompt red", 900.0);
    a.set_background_rate("delayed red", 1100.0);
    const int h2 = a.add_histogram("Proximity Ratio", 50, -0.1, 1.1, "Stoichiometry", 50, -0.1, 1.1);
    for (const auto &p : ph) a.push_photon(p.t, p.micro, p.ch);
    a.flush();

    std::vector<Photon> sel;
    for (const auto &p : ph) if (p.ch >= 0 && p.ch <= 3) sel.push_back(p);
    const long long Ti = static_cast<long long>(T / dt);
    std::vector<std::pair<long long, long long>> ref;
    {
        bool in = false; long long i0 = 0;
        const long long n = static_cast<long long>(sel.size());
        for (long long i = 0; i <= n - m; ++i) {
            if (sel[i + m - 1].t - sel[i].t <= static_cast<unsigned long long>(Ti)) {
                if (!in) { in = true; i0 = i; }
            } else if (in) {
                in = false;
                if (i + m - 2 - i0 + 1 >= L) ref.push_back({i0, i + m - 2});
            }
        }
        if (in && n - i0 >= L) ref.push_back({i0, n - 1});
    }
    StreamingBurstDetector det(m, T, dt);
    det.set_min_photons(L);
    for (const auto &p : sel) det.push_photon(p.t);
    det.flush();
    const auto first = a.table_column("First Photon"), last = a.table_column("Last Photon");
    bool same = first.size() == ref.size() && det.burst_count() == ref.size();
    for (size_t i = 0; same && i < ref.size(); ++i) same = first[i] == ref[i].first && last[i] == ref[i].second;
    check(same && ref.size() > 200, "PIE: bursts equal the batch window and the detector", static_cast<double>(ref.size()));

    const auto dd = a.table_column("S prompt green (photons) | 0-2048");
    const auto da = a.table_column("S prompt red (photons) | 0-2048");
    const auto aa = a.table_column("S delayed red (photons) | 2048-4096");
    const auto pr = a.table_column("Proximity Ratio"), st = a.table_column("Stoichiometry");
    const auto ec = a.table_column("FRET Efficiency"), sc = a.table_column("Stoichiometry (corrected)");
    double worst = 0.0;
    for (size_t b = 0; same && b < ref.size(); ++b) {
        double n_dd = 0, n_da = 0, n_aa = 0;
        for (long long k = ref[b].first; k <= ref[b].second; ++k) {
            const auto &p = sel[static_cast<size_t>(k)];
            const bool green = p.ch == 0 || p.ch == 2, red = p.ch == 1 || p.ch == 3;
            if (green && p.micro < 2048) n_dd++;
            if (red && p.micro < 2048) n_da++;
            if (red && p.micro >= 2048) n_aa++;
        }
        const double span = static_cast<double>(sel[static_cast<size_t>(ref[b].second)].t -
                                                sel[static_cast<size_t>(ref[b].first)].t) * dt;
        const double f_dd = n_dd - 1500.0 * span, f_aa = n_aa - 1100.0 * span;
        const double f_da = (n_da - 900.0 * span) - al * f_dd - de * f_aa;
        const double e_c = (f_da + g * f_dd) > 0 ? f_da / (f_da + g * f_dd) : 0.0;
        const double s_den = g * f_dd + f_da + f_aa / be;
        worst = std::max({worst, std::fabs(n_dd - dd[b]), std::fabs(n_da - da[b]), std::fabs(n_aa - aa[b]),
                          std::fabs((n_dd + n_da ? n_da / (n_dd + n_da) : 0.0) - pr[b]),
                          std::fabs((n_dd + n_da + n_aa ? (n_dd + n_da) / (n_dd + n_da + n_aa) : 0.0) - st[b]),
                          std::fabs(e_c - ec[b]),
                          std::fabs((s_den != 0 ? (g * f_dd + f_da) / s_den : 0.0) - sc[b])});
    }
    check(worst < 1e-9, "PIE: counts, ratios and corrected E/S equal the batch", worst);
    double h = 0, in_range = 0;
    for (double v : a.histogram(h2)) h += v;
    for (size_t b = 0; b < pr.size(); ++b) if (pr[b] >= -0.1 && pr[b] < 1.1 && st[b] >= -0.1 && st[b] < 1.1) in_range++;
    check(h == in_range, "the E-S histogram holds every in-range burst", h);
    check(a.photon_count() == sel.size(), "only declared channels are counted", static_cast<double>(a.photon_count()));

    a.clear();
    for (const auto &p : ph) a.push_photon(p.t, p.micro, p.ch);
    a.flush();
    check(a.table_column("First Photon") == first, "replay after clear() gives the same bursts", 0.0);

    // ---- µs-ALEX: phase windows ------------------------------------------------
    {
        const uint64_t period = 8000;   // 100 µs at 12.5 ns
        StreamingBurstAnalyzer x(dt, micro_dt);
        x.add_detector("green", {0, 2}, {});
        x.add_detector("red", {1, 3}, {});
        x.add_phase_window("Dex", period, 0, period / 2);
        x.add_phase_window("Aex", period, period / 2, period);
        x.set_search(L, m, T);
        for (const auto &p : ph) x.push_photon(p.t, p.micro, p.ch);
        x.flush();
        const auto f = x.table_column("First Photon"), l = x.table_column("Last Photon");
        const auto ar = x.table_column("S Aex red (photons) | 4000-8000");
        double worst_a = f.size() == ref.size() ? 0.0 : 1e9;
        for (size_t b = 0; worst_a == 0.0 && b < f.size(); ++b) {
            double n = 0;
            for (long long k = static_cast<long long>(f[b]); k <= static_cast<long long>(l[b]); ++k) {
                const auto &p = sel[static_cast<size_t>(k)];
                if ((p.ch == 1 || p.ch == 3) && p.t % period >= period / 2) n++;
            }
            worst_a = std::max(worst_a, std::fabs(n - ar[b]));
        }
        check(worst_a == 0.0, "ALEX: phase-window counts equal (macro mod period)", worst_a);
    }

    // ---- MFD: steady-state anisotropy ---------------------------------------------
    {
        StreamingBurstAnalyzer x(dt, micro_dt);
        x.add_detector("green_par", {0}, {});
        x.add_detector("red_par", {1}, {});
        x.add_detector("green_perp", {2}, {});
        x.add_detector("red_perp", {3}, {});
        x.set_search(L, m, T);
        const double G = 1.05, l1 = 0.03, l2 = 0.02;
        x.add_anisotropy("r Experimental (green)", "green_par", "green_perp", G, l1, l2, false);
        for (const auto &p : ph) x.push_photon(p.t, p.micro, p.ch);
        x.flush();
        const auto fp = x.table_column("Number of Photons (green_par)");
        const auto fs = x.table_column("Number of Photons (green_perp)");
        const auto r = x.table_column("r Experimental (green)");
        double worst_r = 0.0;
        for (size_t b = 0; b < r.size(); ++b) {
            const double den = fp[b] * (1 - 3 * l2) + (2 - 3 * l1) * G * fs[b];
            worst_r = std::max(worst_r, std::fabs((den != 0 ? (fp[b] - G * fs[b]) / den : 0.0) - r[b]));
        }
        check(r.size() == ref.size() && worst_r < 1e-12, "MFD: anisotropy equals DecayFit's formula", worst_r);
    }

    // ---- a search on a channel subset --------------------------------------------
    {
        StreamingBurstAnalyzer x(dt, micro_dt);
        x.add_detector("green", {0, 2}, {});
        x.add_detector("red", {1, 3}, {});
        x.set_search_channels({1, 3});
        x.set_search(L, m, T);
        for (const auto &p : ph) x.push_photon(p.t, p.micro, p.ch);
        x.flush();
        const auto ng = x.table_column("Number of Photons (green)");
        const auto dur = x.table_column("Duration (ms)");
        const auto t0 = x.table_column("Mean Macro Time (ms)");
        // green photons counted by time inside each (red-searched) burst
        double worst_s = 0.0;
        for (size_t b = 0; b < ng.size(); ++b) {
            const double lo = (t0[b] - dur[b] / 2) / (dt * 1e3), hi = (t0[b] + dur[b] / 2) / (dt * 1e3);
            double n = 0;
            for (const auto &p : ph)
                if ((p.ch == 0 || p.ch == 2) && static_cast<long long>(p.t) >= std::llround(lo) && static_cast<long long>(p.t) <= std::llround(hi)) n++;
            worst_s = std::max(worst_s, std::fabs(n - ng[b]));
        }
        check(ng.size() > 100 && worst_s == 0.0, "subset search: other detectors counted by time", worst_s);
    }

    // ---- decays, trace, DCBS ---------------------------------------------------------
    {
        StreamingBurstAnalyzer x(dt, micro_dt);
        x.add_detector("green", {0, 2}, {});
        x.add_detector("red", {1, 3}, {});
        x.set_search(L, m, T);
        x.set_decays(64, 4096, true);
        x.set_trace(1e-3, 100);
        for (const auto &p : ph) x.push_photon(p.t, p.micro, p.ch);
        x.flush();
        double sb = 0; for (double v : x.decay("red", true)) sb += v;
        double nb = 0; for (double v : x.table_column("Number of Photons (red)")) nb += v;
        check(sb == nb, "burst decays sum to the bursts' photons", sb);
        double sa = 0; for (double v : x.decay("red", false)) sa += v;
        check(sa == x.stream_photons("red"), "all-photon decays sum to the stream", sa);
        double one = 0; for (double v : x.burst_decay(0, "red")) one += v;
        check(one == x.table_column("Number of Photons (red)")[0], "one burst's decay sums to its photons", one);
        check(x.trace("green").size() == 100, "the trace keeps the newest bins", static_cast<double>(x.trace("green").size()));
        bool raised = false;
        StreamingBurstAnalyzer y(dt, micro_dt);
        try { y.set_search_mode("dcbs"); } catch (const std::invalid_argument &) { raised = true; }
        check(raised, "DCBS raises 'not implemented yet'", 0.0);
    }

    std::printf("%d failure(s)\n", failures);
    return failures;
}
