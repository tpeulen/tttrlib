// StreamingPieBurstAnalyzer.h: live PIE burst analysis agrees with the batch path.
//
// Checks, on a synthetic PIE stream (Poisson background with planted bursts of
// known donor/acceptor/prompt/delay composition, plus photons on a channel no
// detector declares):
//   - the bursts equal the batch sliding window (`TTTR::burst_search_sliding_window`,
//     written out here) run on the channel-selected stream, index for index, and
//     equal StreamingBurstDetector fed the same selected photons;
//   - per-burst detector and window counts equal a direct count over the batch
//     burst's photons (half-open ranges);
//   - raw E/S equal apparent_es and corrected E/S equal corrected_es (written out
//     here) with fixed backgrounds;
//   - the E-S histogram holds every in-range burst once and its marginals agree;
//   - clear() returns to an empty state and the same photons give the same table.
//
//     c++ -std=c++17 -O2 -I modules/streaming/include \
//         -o t test/cpp/test_streaming_pie_burst_analyzer.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-10-26.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "StreamingBurstDetector.h"
#include "StreamingPieBurstAnalyzer.h"

static int failures = 0;
static void check(bool ok, const char *what, double value) {
    std::printf("%s %-66s %.6g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

struct Photon { uint64_t t; uint16_t micro; int ch; };

int main() {
    using tttrlib::StreamingPieBurstAnalyzer;
    using tttrlib::StreamingBurstDetector;

    // ---- a synthetic PIE stream ------------------------------------------------
    // channels: 0 donor, 1 acceptor, 5 undeclared; micro 0..4095, prompt [0, 2048)
    const double dt = 12.5e-9, micro_dt = 16e-12;
    std::mt19937_64 rng(11);
    std::uniform_int_distribution<int> micro_prompt(0, 2047), micro_delay(2048, 4095);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::vector<Photon> ph;
    double t = 0.0;
    for (int b = 0; b < 400; ++b) {
        // background stretch
        const double t_end = t + 0.01 / dt;
        while (true) {
            t += 1.0 + std::floor(1.0 / (1.0 / 2e-4 * dt) * -std::log(1.0 - U(rng)));
            if (t > t_end) break;
            const int ch = U(rng) < 0.1 ? 5 : (U(rng) < 0.5 ? 0 : 1);
            const uint16_t mi = static_cast<uint16_t>(U(rng) < 0.5 ? micro_prompt(rng) : micro_delay(rng));
            ph.push_back({static_cast<uint64_t>(t), mi, ch});
        }
        // a burst: 40..120 photons at ~200 kHz, population E 0.3 or 0.8, S ~0.5
        const int n = 40 + static_cast<int>(U(rng) * 80);
        const double e = (b % 2) ? 0.3 : 0.8;
        for (int k = 0; k < n; ++k) {
            t += 1.0 + std::floor(-std::log(1.0 - U(rng)) * (5e-6 / dt));
            int ch; uint16_t mi;
            if (U(rng) < 0.5) {            // donor excitation
                mi = static_cast<uint16_t>(micro_prompt(rng));
                ch = U(rng) < e ? 1 : 0;
            } else {                       // acceptor excitation
                mi = static_cast<uint16_t>(micro_delay(rng));
                ch = U(rng) < 0.95 ? 1 : 0;
            }
            ph.push_back({static_cast<uint64_t>(t), mi, ch});
        }
    }
    check(ph.size() > 20000, "synthetic stream has photons", static_cast<double>(ph.size()));

    const int L = 20, m = 10;
    const double T = 1e-4;

    auto setup = [&](StreamingPieBurstAnalyzer &a) {
        a.add_detector("green", {0}, {});
        a.add_detector("red", {1}, {});
        a.add_window("prompt", 0, 2048);
        a.add_window("delayed", 2048, 4096);
        a.set_pie_roles("green", "red", "prompt", "delayed");
    };

    StreamingPieBurstAnalyzer a(L, m, T, dt, micro_dt);
    setup(a);
    const double g = 1.2, be = 0.8, al = 0.06, de = 0.04;
    const double bg_dd = 1500.0, bg_da = 900.0, bg_aa = 1100.0;  // counts / s
    a.set_corrections(g, be, al, de);
    a.set_background_rates(bg_dd, bg_da, bg_aa);
    for (const auto &p : ph) a.push_photon(p.t, p.micro, p.ch);
    a.flush();

    // ---- batch reference ----------------------------------------------------------
    std::vector<Photon> sel;
    for (const auto &p : ph) if (p.ch == 0 || p.ch == 1) sel.push_back(p);
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
                const long long i1 = i + m - 2;
                if (i1 - i0 + 1 >= L) ref.push_back({i0, i1});
            }
        }
        if (in && n - 1 - i0 + 1 >= L) ref.push_back({i0, n - 1});
    }
    StreamingBurstDetector det(m, T, dt);
    det.set_min_photons(L);
    for (const auto &p : sel) det.push_photon(p.t);
    det.flush();

    const std::vector<double> first = a.table_column("First Photon");
    const std::vector<double> last = a.table_column("Last Photon");
    check(first.size() == ref.size(), "burst count equals the batch sliding window", static_cast<double>(first.size()));
    check(det.burst_count() == ref.size(), "StreamingBurstDetector agrees on the count", static_cast<double>(det.burst_count()));
    bool same = first.size() == ref.size();
    for (size_t i = 0; same && i < ref.size(); ++i)
        same = first[i] == ref[i].first && last[i] == ref[i].second;
    check(same, "burst boundaries equal the batch, index for index", 0.0);

    // per-burst counts and E/S
    const auto dd = a.table_column("S prompt green (photons) | 0-2048");
    const auto da = a.table_column("S prompt red (photons) | 0-2048");
    const auto aa = a.table_column("S delayed red (photons) | 2048-4096");
    const auto ng = a.table_column("Number of Photons (green)");
    const auto er = a.table_column("Proximity Ratio");
    const auto sr = a.table_column("Stoichiometry");
    const auto ec = a.table_column("FRET Efficiency");
    const auto sc = a.table_column("Stoichiometry (corrected)");
    double worst_count = 0.0, worst_es = 0.0;
    for (size_t b = 0; same && b < ref.size(); ++b) {
        double n_dd = 0, n_da = 0, n_aa = 0, n_g = 0;
        for (long long k = ref[b].first; k <= ref[b].second; ++k) {
            const auto &p = sel[static_cast<size_t>(k)];
            const bool prompt = p.micro < 2048, delay = p.micro >= 2048 && p.micro < 4096;
            if (p.ch == 0) n_g++;
            if (p.ch == 0 && prompt) n_dd++;
            if (p.ch == 1 && prompt) n_da++;
            if (p.ch == 1 && delay) n_aa++;
        }
        worst_count = std::max({worst_count, std::fabs(n_dd - dd[b]), std::fabs(n_da - da[b]),
                                std::fabs(n_aa - aa[b]), std::fabs(n_g - ng[b])});
        // apparent_es
        const double e_raw = (n_dd + n_da) != 0 ? n_da / (n_dd + n_da) : 0.0;
        const double s_raw = (n_dd + n_da + n_aa) != 0 ? (n_dd + n_da) / (n_dd + n_da + n_aa) : 0.0;
        // corrected_es with background = rate x span
        const double span = static_cast<double>(sel[static_cast<size_t>(ref[b].second)].t -
                                                sel[static_cast<size_t>(ref[b].first)].t) * dt;
        const double f_dd = n_dd - bg_dd * span, f_aa = n_aa - bg_aa * span;
        const double f_da = (n_da - bg_da * span) - al * f_dd - de * f_aa;
        const double e_c = (f_da + g * f_dd) > 0 ? f_da / (f_da + g * f_dd) : 0.0;
        const double s_den = g * f_dd + f_da + f_aa / be;
        const double s_c = s_den != 0 ? (g * f_dd + f_da) / s_den : 0.0;
        worst_es = std::max({worst_es, std::fabs(e_raw - er[b]), std::fabs(s_raw - sr[b]),
                             std::fabs(e_c - ec[b]), std::fabs(s_c - sc[b])});
    }
    check(worst_count == 0.0, "detector and window counts equal a direct count", worst_count);
    check(worst_es < 1e-9, "raw and corrected E/S equal apparent_es / corrected_es", worst_es);

    // populations: raw E near 0.3 and 0.8 (background is small)
    int lo = 0, hi = 0;
    for (double e : er) { if (e > 0.15 && e < 0.5) lo++; if (e > 0.65 && e < 0.95) hi++; }
    check(lo > 120 && hi > 120, "both simulated E populations are recovered", lo + hi);

    // histogram: raw by default; every burst with E and S in range counted once
    double in_range = 0;
    for (size_t b = 0; b < er.size(); ++b)
        if (er[b] >= -0.1 && er[b] < 1.1 && sr[b] >= -0.1 && sr[b] < 1.1) in_range++;
    double h = 0; for (double v : a.es_histogram()) h += v;
    double me = 0; for (double v : a.e_marginal()) me += v;
    check(h == in_range, "the E-S histogram holds every in-range burst", h);
    check(me >= h, "the E marginal holds at least the 2-D histogram", me);

    // selection: undeclared channel photons never enter
    check(a.photon_count() == sel.size(), "only declared channels are counted", static_cast<double>(a.photon_count()));

    // clear() and replay give the same table
    a.clear();
    check(a.burst_count() == 0 && a.photon_count() == 0, "clear() empties the state", 0.0);
    for (const auto &p : ph) a.push_photon(p.t, p.micro, p.ch);
    a.flush();
    check(a.table_column("First Photon") == first, "replay after clear() gives the same bursts", 0.0);

    // live background estimate is finite and positive on this stream
    StreamingPieBurstAnalyzer live(L, m, T, dt, micro_dt);
    setup(live);
    for (const auto &p : ph) live.push_photon(p.t, p.micro, p.ch);
    const std::vector<double> bgr = live.background_rates();
    check(bgr[0] > 0 && bgr[1] > 0 && bgr[2] > 0, "live background rates are positive", bgr[0]);

    std::printf("%d failure(s)\n", failures);
    return failures;
}
