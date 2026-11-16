// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_ALTERNATION_FOLD_H
#define TTTRLIB_ALTERNATION_FOLD_H

// Validation: KNOWN-ANSWER 2026-11-16 -- equal two-slot fold vs the numpy
//   transcription t' = floor(t/P)*(P/2) + (t mod P) mod (P/2) (BmrA µs-ALEX
//   H2MM, ~/projects/BmrA/pipeline/h2mm_states.py), exact up to the constant
//   origin of one folded period; phase-from-micro-time vs phase-from-macro-time
//   identical; concatenated files with different phase offsets.
//   test/cpp/test_alternation_fold.cpp, test/python/tttr/test_alternation_fold.py

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>

/**
 * @brief Folds the laser alternation of µs-ALEX data out of the macro-time clock.
 *
 * Under µs-ALEX the lasers alternate with a period P that is long compared to
 * the photon arrival times many analyses resolve: in each period the
 * donor-excitation photons arrive in one time slot and the acceptor-excitation
 * photons in another. Anything that models or correlates photon arrival times
 * on the raw macro-time clock sees that alternation as a property of the
 * molecule. A hidden Markov model of a burst finds states that cycle between
 * donor- and acceptor-excitation photons at the alternation rate, a correlation
 * curve oscillates with period P, and a short time window sees only one laser.
 *
 * Folding maps every alternation period onto the length of one excitation slot
 * and overlays the slots, as if both lasers were on at the same time (the
 * photons then look like PIE data). With the period divided into slots that
 * start at phases s_0 < s_1 < ... (circularly, relative to s_0) and the longest
 * slot length L, a photon at macro time t with alternation phase phi is mapped
 * to
 *
 *     psi = (phi - s_0) mod P        phase relative to the first slot
 *     k   = the slot containing psi
 *     c   = t - psi                  macro time at which its period started
 *     t'  = floor(c * L / P) + (psi - (s_k - s_0)) + L
 *
 * so consecutive periods are L folded ticks apart and the position of a photon
 * inside its slot is kept. For two equal slots at phase 0 this is
 * t' = floor(t / P) * P/2 + (t mod P) mod P/2 + P/2. The constant L at the end
 * keeps the folded time of a photon in the first, partial period of a file
 * non-negative; it shifts every folded time equally and does not change any
 * interval.
 *
 * One folded tick corresponds on average to P / L macro-time ticks of real
 * time (::get_tick_scale): 2 for a 50/50 duty cycle, P / L_max in general.
 * Rates and correlation lags computed on the folded clock are converted to
 * real time with that factor.
 *
 * The phase comes from one of two places. After TTTR::alex_to_microtime the
 * micro time *is* the alternation phase, and the per-photon cycle start
 * c = t - psi is exact even when a container concatenates files whose clocks
 * start at different phases. For a stream that was not converted the phase is
 * (t - period_shift) mod P.
 *
 * The slot layout has to match the excitation windows: every window must lie
 * inside one slot without crossing a slot boundary, or part of it lands at the
 * other end of the folded period. ::from_windows places the slots for a set of
 * windows and also aligns them: the detection windows of the lasers rarely have
 * the same length or the same position in their slot (2026-11 BmrA data: 578-3782
 * and 4278-7762 of 8000, i.e. offsets 578 and 278 in their half periods), so a
 * plain overlay leaves a few µs of every folded period in which only one
 * excitation is gated. A photon-by-photon HMM models that as a state visited once
 * per period (seen on simulated static molecules: an S = 0 state at ~3e5/s). With
 * aligned windows the common window of length l = min(window lengths) is cut from
 * the centre of every window and placed at the centre of the folded period,
 *
 *     x  = psi - lo_k,  lo_k = start of window k + (length_k - l) / 2
 *     t' = floor(c * L / P) + (L - l) / 2 + x + L      for 0 <= x < l,
 *
 * so that at every folded time inside it all excitations are gated.
 * ::in_common_window tells the photons inside; analyses that compare the
 * excitations photon by photon (H2MM) use only those, analyses of one
 * excitation's photons (BVA, PDA) or of whole periods (SVA) use all. Photons
 * outside keep the plain slot position. The folded clock is not monotonic within a period (a late
 * donor-excitation photon folds after an early acceptor-excitation photon);
 * consumers that need sorted times sort the folded times of a burst (stably)
 * and permute the photon attributes with them.
 *
 * What is folded and what is not: quantities that model the arrival process
 * at time scales of the alternation period or below use the folded clock
 * (photon-by-photon HMMs, BVA/SVA windows, burst-wise PDA time windows,
 * correlation at lags below P). Burst search, burst durations and count rates
 * keep the real clock: they describe how long a molecule spent in the focus
 * and how many photons per real second it gave, and a folded duration would
 * be shorter by P / L without meaning anything else.
 */
class AlternationFold {

public:
    AlternationFold() = default;

    /**
     * @brief A fold with explicit slot starts.
     * @param period Alternation period in macro-time ticks (> 0).
     * @param slot_starts Phases in [0, period) at which the slots start. Their
     *        order around the circle is taken from their values; at least one.
     * @throws std::invalid_argument for a non-positive period, no slot, a start
     *         outside [0, period) or two equal starts.
     */
    AlternationFold(long long period, std::vector<int> slot_starts) {
        if (period <= 0) throw std::invalid_argument("alternation period must be positive");
        if (slot_starts.empty()) throw std::invalid_argument("alternation fold needs at least one slot");
        for (int s : slot_starts)
            if (s < 0 || s >= period)
                throw std::invalid_argument("slot start " + std::to_string(s) + " outside [0, period)");
        std::sort(slot_starts.begin(), slot_starts.end());
        if (std::adjacent_find(slot_starts.begin(), slot_starts.end()) != slot_starts.end())
            throw std::invalid_argument("slot starts must be distinct");
        period_ = period;
        starts_ = slot_starts;
        lo_.clear();
        common_ = 0;
        rel_.resize(starts_.size());
        for (size_t k = 0; k < starts_.size(); ++k) rel_[k] = starts_[k] - starts_[0];
        slot_length_ = 0;
        for (size_t k = 0; k < rel_.size(); ++k) {
            const long long end = (k + 1 < rel_.size()) ? rel_[k + 1] : period_;
            slot_length_ = std::max(slot_length_, end - rel_[k]);
        }
    }

    /**
     * @brief n equal slots, the first starting at phase @p offset.
     * @param period Alternation period in macro-time ticks; must be a multiple
     *        of @p n_slots so that every slot has the same length.
     * @param n_slots Number of excitation slots (lasers) per period.
     * @param offset Phase of the first slot start; any integer, taken modulo
     *        the period.
     * @throws std::invalid_argument if the period is not a multiple of n_slots.
     */
    static AlternationFold equal_slots(long long period, int n_slots = 2, long long offset = 0) {
        if (n_slots < 1 || period <= 0 || period % n_slots != 0)
            throw std::invalid_argument("equal slots need a period that is a positive multiple of n_slots");
        const long long L = period / n_slots;
        std::vector<int> s(n_slots);
        for (int k = 0; k < n_slots; ++k) s[k] = static_cast<int>(pmod(offset + k * L, period));
        return AlternationFold(period, s);
    }

    /**
     * @brief Places the slots so that every excitation window lies inside one.
     *
     * Overlapping windows are merged (the detector windows of one laser are
     * usually listed once per detector). If the period divides into as many
     * equal slots as there are merged windows with each window inside its own
     * slot, those are used, with the slot boundaries centred in the gaps the
     * windows leave (the fold is then exactly the duty-cycle-independent
     * P / n_windows). Otherwise each slot boundary is put in the middle of the
     * gap between two windows and the slots differ in length; the folded tick
     * is then P / L_max.
     *
     * @param period Alternation period in macro-time ticks.
     * @param windows Flat list [start_0, stop_0, start_1, stop_1, ...] of
     *        inclusive phase windows in [0, period). A window with start > stop
     *        wraps through phase 0.
     * @throws std::invalid_argument for an odd-length list, no window, a bound
     *         outside [0, period), or windows that together cover the whole
     *         period (no gap to put a slot boundary in) while there are more
     *         than one.
     */
    static AlternationFold from_windows(long long period, std::vector<int> windows) {
        if (period <= 0) throw std::invalid_argument("alternation period must be positive");
        if (windows.empty() || windows.size() % 2 != 0)
            throw std::invalid_argument("windows must be a non-empty flat list of (start, stop) pairs");
        // unwrap to [a, b] with a in [0, P), b >= a, b - a < P
        std::vector<std::pair<long long, long long>> w;
        for (size_t i = 0; i < windows.size(); i += 2) {
            const long long a = windows[i], b = windows[i + 1];
            if (a < 0 || a >= period || b < 0 || b >= period)
                throw std::invalid_argument("window bound outside [0, period)");
            w.emplace_back(a, b >= a ? b : b + period);
        }
        std::sort(w.begin(), w.end());
        // merge on the circle: also a window reaching past P into the first ones
        std::vector<std::pair<long long, long long>> m;
        for (auto& x : w) {
            if (!m.empty() && x.first <= m.back().second + 1) m.back().second = std::max(m.back().second, x.second);
            else m.push_back(x);
        }
        while (m.size() > 1 && m.back().second + 1 >= m.front().first + period) {
            m.front().first = m.back().first - period;
            m.front().second = std::max(m.front().second, m.back().second - period);
            m.pop_back();
        }
        const int n = static_cast<int>(m.size());
        if (n == 1) {
            // one excitation: one slot, starting in the middle of the dark gap
            const long long gap_mid = floordiv(m[0].second + 1 + m[0].first + period, 2);
            return AlternationFold(period, {static_cast<int>(pmod(gap_mid, period))});
        }
        for (int i = 0; i < n; ++i) {
            if (m[i].second - m[i].first + 1 >= period)
                throw std::invalid_argument("excitation windows cover the whole period");
        }
        // equal slots: offset o with o + i*L <= a_i and b_i < o + (i+1)*L for all i
        if (period % n == 0) {
            const long long L = period / n;
            long long lo = -(1LL << 62), hi = (1LL << 62);
            for (int i = 0; i < n; ++i) {
                const long long a = m[i].first, b = m[i].second;
                lo = std::max(lo, b + 1 - (i + 1) * L);
                hi = std::min(hi, a - i * L);
            }
            if (lo <= hi) {
                AlternationFold f = equal_slots(period, n, floordiv(lo + hi, 2));
                f.align(m);
                return f;
            }
        }
        std::vector<int> s(n);
        for (int i = 0; i < n; ++i) {
            const long long prev_end = (i == 0) ? m[n - 1].second - period : m[i - 1].second;
            s[i] = static_cast<int>(pmod(floordiv(prev_end + 1 + m[i].first, 2), period));
        }
        AlternationFold f(period, s);
        f.align(m);
        return f;
    }

    /// Length of the common (aligned) window in macro ticks; 0 when the windows are not aligned.
    long long get_common_window_length() const { return common_; }

    /**
     * @brief Whether a photon of this phase lies in the common window of the aligned excitations.
     * @param phase Alternation phase in [0, period).
     * @return true for every phase when the fold has no aligned windows.
     */
    bool in_common_window(long long phase) const {
        if (common_ <= 0) return true;
        const long long psi = pmod(phase - starts_[0], period_);
        const size_t k = slot_of(psi);
        const long long x = psi - lo_[k];
        return x >= 0 && x < common_;
    }

    /// Alternation period in macro-time ticks (0 for a default-constructed fold).
    long long get_period() const { return period_; }

    /// Slot start phases, ascending.
    std::vector<int> get_slot_starts() const { return starts_; }

    /// Length of the longest slot, in macro-time ticks: the folded period.
    long long get_slot_length() const { return slot_length_; }

    /// Real macro-time ticks per folded tick, P / L (2 for two equal slots).
    double get_tick_scale() const {
        return slot_length_ > 0 ? static_cast<double>(period_) / static_cast<double>(slot_length_) : 1.0;
    }

    /// True when the fold is usable (a period and at least one slot).
    bool is_valid() const { return period_ > 0 && !starts_.empty(); }

    /**
     * @brief Folded time of one photon.
     * @param macro_time Macro time in ticks.
     * @param phase Alternation phase in [0, period): the micro time after
     *        TTTR::alex_to_microtime, or (macro_time - shift) mod period.
     * @return Folded macro time in folded ticks (see the class description).
     * @throws std::invalid_argument for an invalid fold or a phase outside [0, period).
     */
    unsigned long long fold(unsigned long long macro_time, long long phase) const {
        if (!is_valid()) throw std::invalid_argument("alternation fold has no period");
        if (phase < 0 || phase >= period_) throw std::invalid_argument("alternation phase outside [0, period)");
        return fold_unchecked(macro_time, phase);
    }

    /**
     * @brief Folds macro times whose alternation phase is stored as the micro time.
     *
     * The layout after TTTR::alex_to_microtime. Output is malloc'ed (numpy takes
     * ownership in the Python binding).
     *
     * @param output Folded macro times, one per photon, in input order.
     * @param n_output Number of photons.
     * @param macro_times Macro times in ticks.
     * @param n_macrotimes Number of macro times.
     * @param micro_times Alternation phases in [0, period).
     * @param n_microtimes Number of phases; must equal n_macrotimes.
     * @throws std::invalid_argument for unequal lengths, an invalid fold or a
     *         phase outside [0, period) (the micro times are not alternation phases).
     */
    void fold_with_phases(unsigned long long** output, int* n_output,
                          unsigned long long* macro_times, int n_macrotimes,
                          unsigned short* micro_times, int n_microtimes) const {
        if (n_macrotimes != n_microtimes)
            throw std::invalid_argument("macro and micro time arrays differ in length");
        if (!is_valid()) throw std::invalid_argument("alternation fold has no period");
        std::vector<unsigned long long> out(static_cast<size_t>(n_macrotimes));
        for (int i = 0; i < n_macrotimes; ++i) {
            if (micro_times[i] >= period_)
                throw std::invalid_argument("micro time exceeds the alternation period: not an alternation phase");
            out[i] = fold_unchecked(macro_times[i], micro_times[i]);
        }
        hand_out(out, output, n_output);
    }

    /**
     * @brief Folds raw macro times; the phase is (t - period_shift) mod period.
     * @param output Folded macro times, one per photon, in input order.
     * @param n_output Number of photons.
     * @param macro_times Macro times in ticks.
     * @param n_macrotimes Number of macro times.
     * @param period_shift Shift of the alternation phase (as in
     *        TTTR::alex_to_microtime); any integer.
     */
    void fold_macro_times(unsigned long long** output, int* n_output,
                          unsigned long long* macro_times, int n_macrotimes,
                          long long period_shift = 0) const {
        if (!is_valid()) throw std::invalid_argument("alternation fold has no period");
        const unsigned long long P = static_cast<unsigned long long>(period_);
        const unsigned long long sh = static_cast<unsigned long long>(pmod(period_shift, period_));
        std::vector<unsigned long long> out(static_cast<size_t>(n_macrotimes));
        for (int i = 0; i < n_macrotimes; ++i) {
            // reduce first: t spans uint64
            const long long phase = static_cast<long long>((macro_times[i] % P + P - sh) % P);
            out[i] = fold_unchecked(macro_times[i], phase);
        }
        hand_out(out, output, n_output);
    }

    /// Internal: fold without range checks (phase already in [0, period)).
    unsigned long long fold_unchecked(unsigned long long t, long long phase) const {
        const long long psi = pmod(phase - starts_[0], period_);
        const size_t k = slot_of(psi);
        const unsigned long long P = static_cast<unsigned long long>(period_);
        const unsigned long long L = static_cast<unsigned long long>(slot_length_);
        // c + P = t - psi + P >= 0: the + L origin of the class description
        const unsigned long long c = t + (P - static_cast<unsigned long long>(psi));
        // c * L / P without overflow: c spans uint64, L <= P
        const unsigned long long base = (c / P) * L + ((c % P) * L) / P;
        if (common_ > 0) {
            const long long x = psi - lo_[k];
            if (x >= 0 && x < common_)
                return base + static_cast<unsigned long long>((slot_length_ - common_) / 2 + x);
        }
        return base + static_cast<unsigned long long>(psi - rel_[k]);
    }

private:
    long long period_ = 0;
    long long slot_length_ = 0;
    std::vector<int> starts_;
    std::vector<long long> rel_;
    std::vector<long long> lo_;   // per slot: start of the common window, relative to s_0
    long long common_ = 0;        // common window length (0: not aligned)

    size_t slot_of(long long psi) const {
        return static_cast<size_t>(std::upper_bound(rel_.begin(), rel_.end(), psi) - rel_.begin()) - 1;
    }

    // one merged window per slot (unwrapped [a, b]); centre-crop all to the shortest
    void align(const std::vector<std::pair<long long, long long>>& m) {
        if (m.size() != starts_.size() || m.size() < 2) return;
        long long l = period_;
        for (auto& w : m) l = std::min(l, w.second - w.first + 1);
        std::vector<long long> lo(starts_.size(), -1);
        for (auto& w : m) {
            const long long psi_a = pmod(w.first - starts_[0], period_);
            const size_t k = slot_of(psi_a);
            if (lo[k] >= 0) return;  // two windows in one slot: no alignment
            lo[k] = psi_a + (w.second - w.first + 1 - l) / 2;
        }
        lo_ = lo;
        common_ = l;
    }

    static long long pmod(long long a, long long p) { long long r = a % p; return r < 0 ? r + p : r; }
    static long long floordiv(long long a, long long b) { long long q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

    static void hand_out(const std::vector<unsigned long long>& v, unsigned long long** output, int* n_output) {
        *n_output = static_cast<int>(v.size());
        *output = static_cast<unsigned long long*>(std::malloc(std::max<size_t>(v.size(), 1) * sizeof(unsigned long long)));
        if (*output == nullptr) throw std::bad_alloc();
        std::copy(v.begin(), v.end(), *output);
    }
};

#endif // TTTRLIB_ALTERNATION_FOLD_H
