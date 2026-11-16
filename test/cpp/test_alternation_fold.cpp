// SPDX-License-Identifier: BSD-3-Clause
// AlternationFold: the µs-ALEX fold against its definition.
//   c++ -std=c++17 -O2 -I modules/core/include test/cpp/test_alternation_fold.cpp -o /tmp/t && /tmp/t
// Exit status is the number of failed checks.

#include "AlternationFold.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

// the numpy transcription the BmrA H2MM used: t' = floor(t/P)*(P/2) + (t mod P) mod (P/2)
static unsigned long long reference_half_fold(unsigned long long t, unsigned long long P) {
    return (t / P) * (P / 2) + (t % P) % (P / 2);
}

static void equal_two_slots_match_reference() {
    const long long P = 8000;
    AlternationFold f = AlternationFold::equal_slots(P, 2, 0);
    CHECK(f.get_slot_length() == 4000);
    CHECK(f.get_tick_scale() == 2.0);
    std::mt19937_64 rng(7);
    for (int i = 0; i < 100000; ++i) {
        const unsigned long long t = rng() % 10000000000ULL;
        // origin: one folded period
        CHECK(f.fold(t, static_cast<long long>(t % P)) == reference_half_fold(t, P) + 4000);
    }
}

static void phase_from_macro_equals_phase_from_micro() {
    const long long P = 8000;
    const long long shift = 4000;
    AlternationFold f = AlternationFold::from_windows(P, {280, 3800, 4616, 7784});
    std::vector<unsigned long long> t(5000);
    std::vector<unsigned short> phase(t.size());
    std::mt19937_64 rng(11);
    unsigned long long now = 123456;
    for (size_t i = 0; i < t.size(); ++i) {
        now += rng() % 3000;
        t[i] = now;
        phase[i] = static_cast<unsigned short>(((now % P) + P - shift) % P);
    }
    unsigned long long *a = nullptr, *b = nullptr;
    int na = 0, nb = 0;
    f.fold_with_phases(&a, &na, t.data(), (int)t.size(), phase.data(), (int)phase.size());
    f.fold_macro_times(&b, &nb, t.data(), (int)t.size(), shift);
    CHECK(na == nb && na == (int)t.size());
    for (int i = 0; i < na; ++i) CHECK(a[i] == b[i]);
    std::free(a);
    std::free(b);
}

static void windows_place_equal_slots() {
    // 2026-11 BmrA container: delayed 280-3800, prompt 4616-7784 of 8000
    AlternationFold f = AlternationFold::from_windows(8000, {4616, 7784, 280, 3800, 4616, 7784});
    CHECK(f.get_slot_length() == 4000);
    CHECK(f.get_tick_scale() == 2.0);
    std::vector<int> s = f.get_slot_starts();
    CHECK(s.size() == 2);
    // both windows inside a slot: boundaries in the gaps 3801..4615 and 7785..279
    CHECK(s[0] >= 0 && s[0] <= 280);
    CHECK(s[1] > 3800 && s[1] <= 4616);
    // a window wrapping through 0 still gets a slot of its own
    AlternationFold g = AlternationFold::from_windows(8000, {7500, 3000, 3500, 7000});
    std::vector<int> gs = g.get_slot_starts();
    CHECK(g.get_slot_length() == 4000);
    CHECK((gs[0] > 3000 && gs[0] <= 3500) || (gs[1] > 3000 && gs[1] <= 3500));
}

static void unequal_duty_cycle() {
    // 70/30 alternation: no two equal slots contain both windows
    AlternationFold f = AlternationFold::from_windows(10000, {100, 6900, 7100, 9900});
    std::vector<int> s = f.get_slot_starts();
    CHECK(s.size() == 2);
    CHECK(s[0] == 0 && s[1] == 7000);
    CHECK(f.get_slot_length() == 7000);
    CHECK(f.get_tick_scale() > 1.428 && f.get_tick_scale() < 1.429);
    // consecutive periods are one slot length apart
    const unsigned long long t0 = 50000 + 200;
    CHECK(f.fold(t0 + 10000, 200) - f.fold(t0, 200) == 7000);
}

static void one_window_is_identity_up_to_origin() {
    AlternationFold f = AlternationFold::from_windows(1000, {100, 600});
    CHECK(f.get_slot_length() == 1000);
    for (unsigned long long t = 5000; t < 7000; t += 37) {
        const long long phase = static_cast<long long>(t % 1000);
        CHECK(f.fold(t, phase) == t + 1000);
    }
}

static void folded_times_overlay_the_slots() {
    // a photon at phase x in slot 0 and one at phase x + P/2 in slot 1 of the same
    // period fold onto the same tick
    AlternationFold f = AlternationFold::equal_slots(8000, 2, 40);
    for (long long x = 40; x < 4040; x += 101) {
        const unsigned long long t = 8000ULL * 1234 + x;
        CHECK(f.fold(t, x % 8000) == f.fold(t + 4000, (x + 4000) % 8000));
    }
}

static void aligned_windows_share_one_folded_interval() {
    // the 2026-11 BmrA gates differ in length (3169 vs 3521) and position in their slots
    AlternationFold f = AlternationFold::from_windows(8000, {4616, 7784, 280, 3800});
    CHECK(f.get_common_window_length() == 3169);
    const unsigned long long base = 8000ULL * 777;
    // window centres fold onto the same tick, and so do the common windows' edges
    const long long c_d = (280 + 3800) / 2, c_p = (4616 + 7784) / 2;
    CHECK(f.fold(base + c_d, c_d) == f.fold(base + c_p, c_p));
    const long long lo_d = 280 + (3521 - 3169) / 2, lo_p = 4616;
    CHECK(f.fold(base + lo_d, lo_d) == f.fold(base + lo_p, lo_p));
    CHECK(f.in_common_window(lo_d) && f.in_common_window(lo_d + 3168));
    CHECK(!f.in_common_window(lo_d - 1) && !f.in_common_window(lo_d + 3169));
    CHECK(f.in_common_window(4616) && f.in_common_window(7784));
    // common photons of one period stay inside one folded period
    const unsigned long long lo = f.fold(base + lo_d, lo_d);
    CHECK(lo - (f.fold(base + lo_d - 8000, lo_d)) == 4000);
    // plain slots have no common window: everything is inside
    CHECK(AlternationFold::equal_slots(8000, 2, 0).in_common_window(10));
}

static void rejects_bad_input() {
    bool threw = false;
    try { AlternationFold::equal_slots(8001, 2, 0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { AlternationFold f(8000, {0, 0}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { AlternationFold::from_windows(8000, {0, 8000}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    AlternationFold f = AlternationFold::equal_slots(8000, 2, 0);
    try { f.fold(10, 8000); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

int main() {
    equal_two_slots_match_reference();
    phase_from_macro_equals_phase_from_micro();
    windows_place_equal_slots();
    unequal_duty_cycle();
    one_window_is_identity_up_to_origin();
    folded_times_overlay_the_slots();
    aligned_windows_share_one_folded_interval();
    rejects_bad_input();
    if (failures == 0) std::printf("test_alternation_fold: all checks passed\n");
    return failures;
}
