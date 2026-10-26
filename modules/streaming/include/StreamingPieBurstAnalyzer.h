// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_STREAMING_PIE_BURST_ANALYZER_H
#define TTTRLIB_STREAMING_PIE_BURST_ANALYZER_H

// Validation: A/B-TESTED 2026-10-26 -- vs the batch path (channel selection, TTTR::burst_search_sliding_window,
//   per-burst counting as `tttrlib sm` writes it, apparent_es / corrected_es), in arbitrary batch splits;
//   recovers simulated PIE populations. test/python/streaming/test_streaming_pie_burst_analyzer.py.
//   Register: okf/testing/algorithm-validation.md

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tttrlib {

// StreamingPieBurstAnalyzer — live PIE/ALEX burst analysis of a photon stream.
//
// What a live smFRET acquisition displays: bursts found as the photons arrive,
// counted per detector and per PIE window, turned into E and S, and accumulated
// into a 2-D E–S histogram that a viewer polls at display rate. Photons are not
// kept: memory is O(m) for the search plus O(bursts) for the table.
//
// It is the streaming twin of the batch burst table (`tttrlib sm`, which is a
// port of ChiSurf's `generate_burst_dataframe`), and agrees with it burst for
// burst:
//
//   * Photon selection. A photon enters the search when its routing channel
//     belongs to a declared detector — by channel only, as the batch selects
//     with `get_tttr_by_channel` — or every photon when no detector is
//     declared. Photon indices ("First Photon", "Last Photon") count selected
//     photons, as the batch's indices count photons of the selected stream.
//   * Burst rule. The sliding window of `StreamingBurstDetector` and
//     `TTTR::burst_search_sliding_window`: m photons spanning no more than T
//     (compared in integer ticks, T / dt truncated); a failing window ends the
//     burst at the photon before the one just pushed; a burst needs at least L
//     photons. An open burst carries across batches.
//   * Counting. A photon is in detector d when its channel is in d's channels
//     and its micro time lies in one of d's ranges (all micro times when d has
//     none); it is in window w when lo <= micro < hi. Ranges are half-open, as
//     in the batch.
//   * E and S. From the PIE roles: F_DD = donor detector in the prompt window,
//     F_DA = acceptor in prompt, F_AA = acceptor in the delay window (AD, the
//     donor in delay, is counted too). Raw values are `apparent_es`; corrected
//     ones are `corrected_es` (Hellenkamp 2018) with gamma, beta, alpha, delta
//     and a per-burst background of rate x burst duration.
//
// Background is estimated live from the photons *between* bursts, per PIE
// stream: (photons of the stream so far - photons of the stream in bursts) /
// (elapsed time - time in bursts). Each burst is corrected with the estimate at
// the moment it closes, which is what a live view can know; fixed rates can be
// set instead (`set_background_rates`). Raw E and S never subtract anything.
//
// Usage:
//   StreamingPieBurstAnalyzer a(20, 10, 5e-4, 12.5e-9, 1.6e-11); // L, m, T[s], dt_macro[s], dt_micro[s]
//   a.add_detector("green", {0, 2}, {});         // channels, flat [lo0, hi0, lo1, hi1, ...]
//   a.add_detector("red", {1, 3}, {});
//   a.add_window("prompt", 0, 2048);
//   a.add_window("delayed", 2048, 4096);
//   a.set_pie_roles("green", "red", "prompt", "delayed");
//   for each chunk: a.push_photons(macro, micro, channel, n);
//   a.flush();
//   a.es_histogram(); a.table_names(); a.table_column("Stoichiometry");
//
class StreamingPieBurstAnalyzer {
public:
    StreamingPieBurstAnalyzer(
        int min_photons = 20,
        int window_photons = 10,
        double window_time = 5e-4,
        double macro_time_resolution = 1.0,
        double micro_time_resolution = 0.0
    ) : L_(min_photons), m_(window_photons), T_(window_time),
        dt_(macro_time_resolution), micro_dt_(micro_time_resolution) {
        validate();
        set_histogram(50, -0.1, 1.1, 50, -0.1, 1.1, false);
    }

    // ---- setup (before the first photon) ------------------------------------

    /// Declare a detector: routing channels and micro-time ranges, the ranges
    /// flat as [lo0, hi0, lo1, hi1, ...] (half-open); empty means every micro time.
    void add_detector(const std::string& name, const std::vector<int>& channels,
                      const std::vector<int>& micro_time_ranges) {
        require_setup_phase("add_detector");
        if (detectors_.size() >= 32)
            throw std::invalid_argument("StreamingPieBurstAnalyzer: at most 32 detectors");
        if (micro_time_ranges.size() % 2 != 0)
            throw std::invalid_argument("StreamingPieBurstAnalyzer: micro_time_ranges must be lo/hi pairs");
        Detector d;
        d.name = name;
        d.channels = channels;
        for (size_t i = 0; i < micro_time_ranges.size(); i += 2)
            d.ranges.push_back({micro_time_ranges[i], micro_time_ranges[i + 1]});
        detectors_.push_back(d);
        for (int c : channels) {
            if (c < 0 || c > 255)
                throw std::invalid_argument("StreamingPieBurstAnalyzer: channel out of range 0..255");
            used_[c] = true;
        }
        any_detector_ = true;
    }

    /// Declare a micro-time window [lo, hi).
    void add_window(const std::string& name, int lo, int hi) {
        require_setup_phase("add_window");
        if (windows_.size() >= 32)
            throw std::invalid_argument("StreamingPieBurstAnalyzer: at most 32 windows");
        windows_.push_back({name, lo, hi});
    }

    /// Name the detectors and windows E and S are computed from.
    void set_pie_roles(const std::string& donor_detector, const std::string& acceptor_detector,
                       const std::string& prompt_window, const std::string& delay_window) {
        require_setup_phase("set_pie_roles");
        donor_ = index_of_detector(donor_detector);
        acceptor_ = index_of_detector(acceptor_detector);
        prompt_ = index_of_window(prompt_window);
        delay_ = delay_window.empty() ? -1 : index_of_window(delay_window);
    }

    /// Accurate-FRET correction factors (Hellenkamp 2018). Defaults: none.
    void set_corrections(double gamma, double beta, double alpha, double delta) {
        if (!(beta != 0.0))
            throw std::invalid_argument("StreamingPieBurstAnalyzer: beta must be non-zero");
        gamma_ = gamma; beta_ = beta; alpha_ = alpha; delta_ = delta;
        corrected_ = true;
    }

    /// Fixed background rates in counts per second for DD, DA and AA, replacing
    /// the live estimate. Negative values return to the live estimate.
    void set_background_rates(double dd, double da, double aa) {
        if (dd < 0.0 || da < 0.0 || aa < 0.0) { fixed_bg_ = false; return; }
        fixed_bg_ = true;
        fixed_bg_rate_[0] = dd; fixed_bg_rate_[1] = da; fixed_bg_rate_[2] = aa;
        corrected_ = true;
    }

    /// Histogram geometry and which E/S it accumulates (raw or corrected).
    /// Changing it clears the histogram and re-bins the bursts recorded so far.
    void set_histogram(int n_e, double e_min, double e_max,
                       int n_s, double s_min, double s_max, bool use_corrected) {
        if (n_e < 1 || n_s < 1 || !(e_max > e_min) || !(s_max > s_min))
            throw std::invalid_argument("StreamingPieBurstAnalyzer: bad histogram geometry");
        n_e_ = n_e; e_min_ = e_min; e_max_ = e_max;
        n_s_ = n_s; s_min_ = s_min; s_max_ = s_max;
        hist_corrected_ = use_corrected;
        rebin();
    }

    // ---- photons --------------------------------------------------------------

    /// Feed one photon. Macro times must not decrease.
    /// @return true iff this photon closed a burst that was recorded.
    bool push_photon(uint64_t macro_time, uint16_t micro_time, int channel) {
        if (!started_) start();
        if (any_detector_ && (channel < 0 || channel > 255 || !used_[channel])) return false;

        const uint32_t dmask = detector_mask(micro_time, channel);
        const uint32_t wmask = window_mask(micro_time);
        const uint64_t j = n_selected_;          // index of this photon among the selected
        Slot& s = ring_[static_cast<size_t>(j % static_cast<uint64_t>(m_))];
        s.macro = macro_time; s.micro = micro_time; s.dmask = dmask; s.wmask = wmask;
        n_selected_++;
        if (n_selected_ == 1) first_macro_ = macro_time;
        last_macro_ = macro_time;
        count_total(dmask, wmask);

        if (n_selected_ < static_cast<uint64_t>(m_)) { prev_macro_ = macro_time; return false; }
        // Oldest photon of the m-photon window ending at this one: the slot the
        // next photon will overwrite.
        const uint64_t t_back =
            ring_[static_cast<size_t>(n_selected_ % static_cast<uint64_t>(m_))].macro;
        const bool in_window = (macro_time - t_back) <= ticks_;

        bool closed = false;
        if (in_window && !in_burst_) {
            in_burst_ = true;
            open_burst(static_cast<int64_t>(j) - m_ + 1);
            for (uint64_t k = j - static_cast<uint64_t>(m_) + 1; k <= j; ++k)
                add_to_burst(static_cast<int64_t>(k), ring_[static_cast<size_t>(k % static_cast<uint64_t>(m_))]);
        } else if (in_window && in_burst_) {
            add_to_burst(static_cast<int64_t>(j), s);
        } else if (!in_window && in_burst_) {
            in_burst_ = false;
            closed = close_burst();
        }
        prev_macro_ = macro_time;
        return closed;
    }

    /// Feed a chunk; `micro_times` and `channels` may be null (0 / channel 0).
    void push_photons(const uint64_t* macro_times, const uint16_t* micro_times,
                      const int* channels, int n) {
        for (int i = 0; i < n; ++i)
            push_photon(macro_times[i], micro_times ? micro_times[i] : 0,
                        channels ? channels[i] : 0);
    }

    /// Close a burst still open at the end of the stream.
    void flush() {
        if (!in_burst_) return;
        in_burst_ = false;
        close_burst();
    }

    /// Forget every photon and burst; the setup is kept.
    void clear() {
        started_ = false;
        in_burst_ = false;
        n_selected_ = 0;
        first_macro_ = last_macro_ = prev_macro_ = 0;
        std::fill(total_wd_.begin(), total_wd_.end(), 0);
        std::fill(burst_wd_.begin(), burst_wd_.end(), 0);
        burst_ticks_ = 0;
        rows_.clear();
        revision_++;
        rebin();
    }

    // ---- live queries -----------------------------------------------------------

    size_t burst_count() const { return rows_.size(); }
    size_t photon_count() const { return static_cast<size_t>(n_selected_); }
    /// Bumped whenever a burst is recorded or the state is reset: poll it.
    uint64_t revision() const { return revision_; }
    double elapsed_seconds() const {
        return n_selected_ ? static_cast<double>(last_macro_ - first_macro_) * dt_ : 0.0;
    }
    double burst_rate_hz() const {
        const double t = elapsed_seconds();
        return t > 0.0 ? static_cast<double>(rows_.size()) / t : 0.0;
    }
    /// Live background estimate, counts per second: [DD, DA, AA, AD].
    std::vector<double> background_rates() const {
        std::vector<double> r(4, 0.0);
        for (int k = 0; k < 4; ++k) r[k] = stream_bg_rate(k) / dt_;
        return r;
    }
    int histogram_n_e() const { return n_e_; }
    int histogram_n_s() const { return n_s_; }
    std::vector<double> histogram_e_edges() const { return edges(n_e_, e_min_, e_max_); }
    std::vector<double> histogram_s_edges() const { return edges(n_s_, s_min_, s_max_); }
    /// Row-major (n_e, n_s): counts of bursts with E in bin i and S in bin j.
    const std::vector<double>& es_histogram() const { return hist_; }
    const std::vector<double>& e_marginal() const { return e_marg_; }
    const std::vector<double>& s_marginal() const { return s_marg_; }

    // ---- the burst table ----------------------------------------------------------

    /// Column names, in the batch table's order.
    std::vector<std::string> table_names() const {
        std::vector<std::string> n = {"First Photon", "Last Photon", "Duration (ms)",
                                      "Mean Macro Time (ms)", "Number of Photons",
                                      "Count Rate (KHz)"};
        for (const auto& d : detectors_) {
            n.push_back("First Photon (" + d.name + ")");
            n.push_back("Last Photon (" + d.name + ")");
            n.push_back("Duration (" + d.name + ") (ms)");
            n.push_back("Mean Macrotime (" + d.name + ") (ms)");
            n.push_back("Number of Photons (" + d.name + ")");
            n.push_back(capitalize(d.name) + " Count Rate (KHz)");
        }
        for (const auto& w : windows_)
            for (const auto& d : detectors_) {
                const std::string stream = "S " + w.name + " " + d.name;
                const std::string range = " | " + std::to_string(w.lo) + "-" + std::to_string(w.hi);
                n.push_back(stream + " (kHz)" + range);
                n.push_back(stream + " (photons)" + range);
                n.push_back(stream + " (ms)" + range);
            }
        for (const auto& d : detectors_) n.push_back("Mean Microtime (" + d.name + ") (ns)");
        if (has_roles()) {
            n.push_back("Proximity Ratio");
            if (delay_ >= 0) n.push_back("Stoichiometry");
            if (corrected_) {
                n.push_back("FRET Efficiency");
                if (delay_ >= 0) n.push_back("Stoichiometry (corrected)");
            }
        }
        return n;
    }

    /// One column of the burst table, as doubles (integer columns are exact).
    std::vector<double> table_column(const std::string& name) const {
        const std::vector<std::string> names = table_names();
        size_t c = 0;
        while (c < names.size() && names[c] != name) ++c;
        if (c == names.size())
            throw std::invalid_argument("StreamingPieBurstAnalyzer: no column '" + name + "'");
        std::vector<double> out(rows_.size());
        for (size_t r = 0; r < rows_.size(); ++r) out[r] = rows_[r].values[c];
        return out;
    }

    int min_photons() const { return L_; }
    int window_photons() const { return m_; }
    double window_time() const { return T_; }
    double macro_time_resolution() const { return dt_; }
    double micro_time_resolution() const { return micro_dt_; }

private:
    struct Detector {
        std::string name;
        std::vector<int> channels;
        std::vector<std::pair<int, int>> ranges;
    };
    struct Window { std::string name; int lo, hi; };
    struct Slot { uint64_t macro = 0; uint16_t micro = 0; uint32_t dmask = 0, wmask = 0; };
    struct Row { std::vector<double> values; double e_raw, s_raw, e_cor, s_cor; };

    // parameters
    int L_, m_;
    double T_, dt_, micro_dt_;
    uint64_t ticks_ = 0;
    std::vector<Detector> detectors_;
    std::vector<Window> windows_;
    bool used_[256] = {false};
    bool any_detector_ = false;
    int donor_ = -1, acceptor_ = -1, prompt_ = -1, delay_ = -1;
    bool corrected_ = false;
    double gamma_ = 1.0, beta_ = 1.0, alpha_ = 0.0, delta_ = 0.0;
    bool fixed_bg_ = false;
    double fixed_bg_rate_[3] = {0.0, 0.0, 0.0};
    int n_e_ = 50, n_s_ = 50;
    double e_min_ = -0.1, e_max_ = 1.1, s_min_ = -0.1, s_max_ = 1.1;
    bool hist_corrected_ = false;

    // state
    bool started_ = false;
    std::vector<Slot> ring_;
    uint64_t n_selected_ = 0, first_macro_ = 0, last_macro_ = 0, prev_macro_ = 0;
    bool in_burst_ = false;
    std::vector<int64_t> total_wd_, burst_wd_;   // per window x detector, photons
    uint64_t burst_ticks_ = 0;                   // summed span of recorded bursts
    // the open burst
    int64_t b_start_ = 0, b_end_ = 0;
    uint64_t b_t0_ = 0, b_t1_ = 0;
    std::vector<int64_t> bd_n_, bd_first_, bd_last_;
    std::vector<uint64_t> bd_t0_, bd_t1_;
    std::vector<double> bd_micro_;
    std::vector<int64_t> bwd_n_;
    std::vector<uint64_t> bwd_t0_, bwd_t1_;
    // results
    std::vector<Row> rows_;
    uint64_t revision_ = 0;
    std::vector<double> hist_, e_marg_, s_marg_;

    static constexpr double kSentinel = -1.0;

    void validate() {
        if (m_ < 1) throw std::invalid_argument("StreamingPieBurstAnalyzer: window_photons must be >= 1");
        if (!(dt_ > 0.0))
            throw std::invalid_argument("StreamingPieBurstAnalyzer: macro_time_resolution must be > 0");
        if (!(T_ > 0.0)) throw std::invalid_argument("StreamingPieBurstAnalyzer: window_time must be > 0");
        const double t = T_ / dt_;
        ticks_ = (t >= 0.0) ? static_cast<uint64_t>(t) : 0;
    }
    void require_setup_phase(const char* what) const {
        if (started_)
            throw std::logic_error(std::string("StreamingPieBurstAnalyzer: ") + what +
                                   " after the first photon; call clear() first");
    }
    int index_of_detector(const std::string& n) const {
        for (size_t i = 0; i < detectors_.size(); ++i) if (detectors_[i].name == n) return static_cast<int>(i);
        throw std::invalid_argument("StreamingPieBurstAnalyzer: no detector '" + n + "'");
    }
    int index_of_window(const std::string& n) const {
        for (size_t i = 0; i < windows_.size(); ++i) if (windows_[i].name == n) return static_cast<int>(i);
        throw std::invalid_argument("StreamingPieBurstAnalyzer: no window '" + n + "'");
    }
    bool has_roles() const { return donor_ >= 0 && acceptor_ >= 0 && prompt_ >= 0; }
    static std::string capitalize(const std::string& s) {
        std::string out = s;
        for (size_t i = 0; i < out.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(out[i]);
            out[i] = static_cast<char>(i == 0 ? std::toupper(c) : std::tolower(c));
        }
        return out;
    }
    static std::vector<double> edges(int n, double lo, double hi) {
        std::vector<double> e(static_cast<size_t>(n) + 1);
        for (int i = 0; i <= n; ++i) e[static_cast<size_t>(i)] = lo + (hi - lo) * i / n;
        return e;
    }

    void start() {
        started_ = true;
        ring_.assign(static_cast<size_t>(m_), Slot());
        const size_t nd = detectors_.size(), nw = windows_.size();
        total_wd_.assign(nw * nd, 0);
        burst_wd_.assign(nw * nd, 0);
        bd_n_.assign(nd, 0); bd_first_.assign(nd, -1); bd_last_.assign(nd, -1);
        bd_t0_.assign(nd, 0); bd_t1_.assign(nd, 0); bd_micro_.assign(nd, 0.0);
        bwd_n_.assign(nw * nd, 0); bwd_t0_.assign(nw * nd, 0); bwd_t1_.assign(nw * nd, 0);
    }

    uint32_t detector_mask(uint16_t micro, int channel) const {
        uint32_t m = 0;
        for (size_t d = 0; d < detectors_.size(); ++d) {
            const Detector& det = detectors_[d];
            bool ch = false;
            for (int c : det.channels) if (c == channel) { ch = true; break; }
            if (!ch) continue;
            bool in = det.ranges.empty();
            for (const auto& r : det.ranges)
                if (micro >= r.first && micro < r.second) { in = true; break; }
            if (in) m |= (1u << d);
        }
        return m;
    }
    uint32_t window_mask(uint16_t micro) const {
        uint32_t m = 0;
        for (size_t w = 0; w < windows_.size(); ++w)
            if (micro >= windows_[w].lo && micro < windows_[w].hi) m |= (1u << w);
        return m;
    }
    void count_total(uint32_t dmask, uint32_t wmask) {
        const size_t nd = detectors_.size();
        if (!dmask || !wmask) return;
        for (size_t w = 0; w < windows_.size(); ++w) {
            if (!(wmask & (1u << w))) continue;
            for (size_t d = 0; d < nd; ++d)
                if (dmask & (1u << d)) total_wd_[w * nd + d]++;
        }
    }

    void open_burst(int64_t start) {
        b_start_ = start; b_end_ = start - 1;
        std::fill(bd_n_.begin(), bd_n_.end(), 0);
        std::fill(bd_first_.begin(), bd_first_.end(), -1);
        std::fill(bd_last_.begin(), bd_last_.end(), -1);
        std::fill(bd_micro_.begin(), bd_micro_.end(), 0.0);
        std::fill(bwd_n_.begin(), bwd_n_.end(), 0);
    }
    void add_to_burst(int64_t idx, const Slot& s) {
        if (idx == b_start_) b_t0_ = s.macro;
        b_end_ = idx; b_t1_ = s.macro;
        const size_t nd = detectors_.size();
        for (size_t d = 0; d < nd; ++d) {
            if (!(s.dmask & (1u << d))) continue;
            if (bd_n_[d] == 0) { bd_first_[d] = idx; bd_t0_[d] = s.macro; }
            bd_last_[d] = idx; bd_t1_[d] = s.macro;
            bd_n_[d]++;
            bd_micro_[d] += s.micro;
            for (size_t w = 0; w < windows_.size(); ++w) {
                if (!(s.wmask & (1u << w))) continue;
                const size_t k = w * nd + d;
                if (bwd_n_[k] == 0) bwd_t0_[k] = s.macro;
                bwd_t1_[k] = s.macro;
                bwd_n_[k]++;
            }
        }
    }

    // PIE stream k (0 DD, 1 DA, 2 AA, 3 AD) as (window, detector), or -1.
    long stream_index(int k) const {
        if (!has_roles()) return -1;
        const size_t nd = detectors_.size();
        int w = -1, d = -1;
        switch (k) {
            case 0: w = prompt_; d = donor_; break;
            case 1: w = prompt_; d = acceptor_; break;
            case 2: w = delay_; d = acceptor_; break;
            default: w = delay_; d = donor_; break;
        }
        if (w < 0 || d < 0) return -1;
        return static_cast<long>(static_cast<size_t>(w) * nd + static_cast<size_t>(d));
    }
    // Background rate of stream k, photons per macro-time tick.
    double stream_bg_rate(int k) const {
        if (fixed_bg_) return k < 3 ? fixed_bg_rate_[k] * dt_ : 0.0;
        const long i = stream_index(k);
        if (i < 0 || n_selected_ == 0) return 0.0;
        const uint64_t span = last_macro_ - first_macro_;
        if (span <= burst_ticks_) return 0.0;
        const double outside = static_cast<double>(span - burst_ticks_);
        const double n = static_cast<double>(total_wd_[static_cast<size_t>(i)] - burst_wd_[static_cast<size_t>(i)]);
        return n > 0.0 ? n / outside : 0.0;
    }

    bool close_burst() {
        const int64_t count = b_end_ - b_start_ + 1;
        if (count < L_ || count <= 0) return false;
        const size_t nd = detectors_.size(), nw = windows_.size();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double ms = dt_ * 1e3;
        const double micro_ns = micro_dt_ > 0.0 ? micro_dt_ * 1e9 : 0.0;

        Row row;
        std::vector<double>& v = row.values;
        const double dur = static_cast<double>(b_t1_ - b_t0_) * ms;
        v.push_back(static_cast<double>(b_start_));
        v.push_back(static_cast<double>(b_end_));
        v.push_back(dur);
        v.push_back((static_cast<double>(b_t1_) + static_cast<double>(b_t0_)) / 2.0 * ms);
        v.push_back(static_cast<double>(count));
        v.push_back(dur > 0.0 ? static_cast<double>(count) / dur : nan);
        for (size_t d = 0; d < nd; ++d) {
            if (bd_n_[d] == 0) {
                v.insert(v.end(), {-1.0, -1.0, kSentinel, kSentinel, 0.0, kSentinel});
                continue;
            }
            const double d_ms = static_cast<double>(bd_t1_[d] - bd_t0_[d]) * ms;
            v.push_back(static_cast<double>(bd_first_[d]));
            v.push_back(static_cast<double>(bd_last_[d]));
            v.push_back(d_ms);
            v.push_back((static_cast<double>(bd_t1_[d]) + static_cast<double>(bd_t0_[d])) / 2.0 * ms);
            v.push_back(static_cast<double>(bd_n_[d]));
            v.push_back(d_ms > 0.0 ? static_cast<double>(bd_n_[d]) / d_ms : nan);
        }
        for (size_t w = 0; w < nw; ++w)
            for (size_t d = 0; d < nd; ++d) {
                const size_t k = w * nd + d;
                if (bwd_n_[k] == 0) { v.insert(v.end(), {kSentinel, 0.0, kSentinel}); continue; }
                const double d_ms = static_cast<double>(bwd_t1_[k] - bwd_t0_[k]) * ms;
                v.push_back(d_ms > 0.0 ? static_cast<double>(bwd_n_[k]) / d_ms : nan);
                v.push_back(static_cast<double>(bwd_n_[k]));
                v.push_back(d_ms);
            }
        for (size_t d = 0; d < nd; ++d)
            v.push_back(bd_n_[d] > 0 && micro_ns > 0.0
                        ? bd_micro_[d] / static_cast<double>(bd_n_[d]) * micro_ns : kSentinel);

        // this burst's photons and span no longer count as background
        for (size_t k = 0; k < nw * nd; ++k) burst_wd_[k] += bwd_n_[k];
        burst_ticks_ += b_t1_ - b_t0_;

        row.e_raw = row.s_raw = row.e_cor = row.s_cor = 0.0;
        if (has_roles()) {
            const auto cnt = [&](int k) {
                const long i = stream_index(k);
                return i < 0 ? 0.0 : static_cast<double>(bwd_n_[static_cast<size_t>(i)]);
            };
            const double i_dd = cnt(0), i_da = cnt(1), i_aa = cnt(2);
            // apparent_es
            const double ed = i_dd + i_da;
            row.e_raw = ed != 0.0 ? i_da / ed : 0.0;
            const double sd = i_dd + i_da + i_aa;
            row.s_raw = sd != 0.0 ? ed / sd : 0.0;
            v.push_back(row.e_raw);
            if (delay_ >= 0) v.push_back(row.s_raw);
            if (corrected_) {
                // corrected_es with background = rate x burst duration
                const double span = static_cast<double>(b_t1_ - b_t0_);
                const double f_dd = i_dd - stream_bg_rate(0) * span;
                const double f_aa = (delay_ >= 0) ? i_aa - stream_bg_rate(2) * span : 0.0;
                const double f_da = (i_da - stream_bg_rate(1) * span) - alpha_ * f_dd - delta_ * f_aa;
                const double e_den = f_da + gamma_ * f_dd;
                row.e_cor = e_den > 0.0 ? f_da / e_den : 0.0;
                const double s_den = gamma_ * f_dd + f_da + f_aa / beta_;
                row.s_cor = s_den != 0.0 ? (gamma_ * f_dd + f_da) / s_den : 0.0;
                v.push_back(row.e_cor);
                if (delay_ >= 0) v.push_back(row.s_cor);
            }
        }
        rows_.push_back(std::move(row));
        accumulate(rows_.back());
        revision_++;
        return true;
    }

    static int bin_of(double x, int n, double lo, double hi) {
        if (!(x >= lo) || !(x < hi)) return -1;
        int i = static_cast<int>((x - lo) / (hi - lo) * n);
        return i >= n ? n - 1 : i;
    }
    void accumulate(const Row& r) {
        if (!has_roles()) return;
        const bool cor = hist_corrected_ && corrected_;
        const double e = cor ? r.e_cor : r.e_raw;
        const double s = cor ? r.s_cor : r.s_raw;
        const int ie = bin_of(e, n_e_, e_min_, e_max_);
        const int is = bin_of(s, n_s_, s_min_, s_max_);
        if (ie >= 0) e_marg_[static_cast<size_t>(ie)] += 1.0;
        if (is >= 0) s_marg_[static_cast<size_t>(is)] += 1.0;
        if (ie >= 0 && is >= 0) hist_[static_cast<size_t>(ie) * static_cast<size_t>(n_s_) + static_cast<size_t>(is)] += 1.0;
    }
    void rebin() {
        hist_.assign(static_cast<size_t>(n_e_) * static_cast<size_t>(n_s_), 0.0);
        e_marg_.assign(static_cast<size_t>(n_e_), 0.0);
        s_marg_.assign(static_cast<size_t>(n_s_), 0.0);
        for (const auto& r : rows_) accumulate(r);
        revision_++;
    }
};

} // namespace tttrlib

#endif // TTTRLIB_STREAMING_PIE_BURST_ANALYZER_H
