// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_STREAMING_BURST_ANALYZER_H
#define TTTRLIB_STREAMING_BURST_ANALYZER_H

// Validation: A/B-TESTED 2026-10-26 -- vs the batch path (channel selection, TTTR::burst_search_sliding_window,
//   per-burst counting as `tttr sm` writes it, apparent_es / corrected_es, DecayFit's steady-state anisotropy),
//   in arbitrary batch splits, for single-colour, two-colour, PIE, µs-ALEX, MFD and three-colour PIE setups;
//   recovers simulated PIE populations. test/python/streaming/test_streaming_burst_analyzer.py,
//   test/cpp/test_streaming_burst_analyzer.cpp. Register: okf/testing/algorithm-validation.md

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tttrlib {

// StreamingBurstAnalyzer — live burst analysis of a photon stream, for any
// detection scheme.
//
// What a live single-molecule acquisition displays, computed as the photons
// arrive and without keeping them: bursts, their per-stream photon counts,
// quantities derived from those counts (proximity ratio, stoichiometry,
// corrected FRET efficiency, anisotropy, mean micro time), and histograms of
// any of those quantities that a viewer polls at display rate. Memory is O(m)
// for the search plus O(bursts) for the table.
//
// The configuration is the shared ChiSurf/CMC channel definition, so one class
// covers single colour, two colour, PIE, µs-ALEX, MFD and three-colour PIE;
// those differ only in their setup (the Python `from_setup(setup, preset=...)`
// builds each):
//
//   * Detectors: a name, routing channels and optional half-open micro-time
//     ranges. A photon is in a detector when its channel is listed and its
//     micro time lies in one of the ranges (any micro time when there are none).
//   * Windows: half-open micro-time ranges (PIE: prompt / delayed), or
//     macro-time *phase* ranges of an alternation period (µs-ALEX), in ticks.
//   * Streams: every (window, detector) pair, named "<window> <detector>", and
//     every detector over all micro times, named "<detector>". Each burst is
//     counted in every stream; each stream has a live background rate.
//   * Search: a configurable stage. The all-photon sliding window — m photons
//     spanning no more than T, the rule of `StreamingBurstDetector` and
//     `TTTR::burst_search_sliding_window`, an open burst carried across chunks —
//     runs on the search channels (default: every detector channel, which is
//     what the batch selects with `get_tttr_by_channel`). Photons of detectors
//     outside the search channels are counted when their macro time lies within
//     the burst. DCBS is declared but not implemented yet (it raises).
//   * Quantities: a small declarative set evaluated per burst — `add_ratio`,
//     `add_corrected_fret` (accurate FRET, Hellenkamp 2018), `add_anisotropy`
//     (DecayFit's steady-state anisotropy), `add_mean_microtime` — each a column
//     of the burst table.
//   * Accumulators: any number of 1-D and 2-D histograms over any table column
//     (E–S is one), a per-stream count-rate trace and per-stream decays (all
//     photons, and burst photons only — the sub-ensemble lifetime hook), plus
//     optionally one decay per burst and stream.
//
// With the default search (every detector channel) the burst table equals the
// batch table — `tttr sm`, ChiSurf's `generate_burst_dataframe` — column for
// column (names, order, sentinels), for any chunking of the photons.
//
// Background of a stream: (photons of the stream so far - photons of the stream
// in recorded bursts) / (elapsed time - time in recorded bursts), evaluated when
// a burst closes; `set_background_rate(stream, cps)` fixes it instead. Only the
// quantities that ask for it subtract background.
//
// Usage (C++):
//   StreamingBurstAnalyzer a(12.5e-9, 16e-12);         // macro, micro resolution [s]
//   a.add_detector("green", {0}, {});
//   a.add_detector("red", {1}, {});
//   a.add_window("prompt", 0, 2048);
//   a.add_window("delayed", 2048, 4096);
//   a.set_search(20, 10, 5e-4);                         // L, m, T [s]
//   a.add_ratio("Proximity Ratio", {"prompt red"}, {"prompt green", "prompt red"}, false);
//   a.add_ratio("Stoichiometry", {"prompt green", "prompt red"},
//               {"prompt green", "prompt red", "delayed red"}, false);
//   a.add_histogram("Proximity Ratio", 50, -0.1, 1.1, "Stoichiometry", 50, -0.1, 1.1);
//   for each chunk: a.push_photons(macro, micro, channel, n);
//   a.flush();
//
class StreamingBurstAnalyzer {
public:
    explicit StreamingBurstAnalyzer(double macro_time_resolution = 1.0,
                                    double micro_time_resolution = 0.0)
        : dt_(macro_time_resolution), micro_dt_(micro_time_resolution) {
        if (!(dt_ > 0.0))
            throw std::invalid_argument("StreamingBurstAnalyzer: macro_time_resolution must be > 0");
        set_search(20, 10, 5e-4);
    }

    // ---- setup: detectors, windows, search (before the first photon) ----------

    void add_detector(const std::string& name, const std::vector<int>& channels,
                      const std::vector<int>& micro_time_ranges) {
        require_setup_phase("add_detector");
        if (detectors_.size() >= 32)
            throw std::invalid_argument("StreamingBurstAnalyzer: at most 32 detectors");
        if (micro_time_ranges.size() % 2 != 0)
            throw std::invalid_argument("StreamingBurstAnalyzer: micro_time_ranges must be lo/hi pairs");
        Detector d;
        d.name = name;
        d.channels = channels;
        for (size_t i = 0; i < micro_time_ranges.size(); i += 2)
            d.ranges.push_back({micro_time_ranges[i], micro_time_ranges[i + 1]});
        for (int c : channels) check_channel(c);
        detectors_.push_back(d);
    }

    /// A micro-time window [lo, hi) (PIE: prompt, delayed).
    void add_window(const std::string& name, int lo, int hi) {
        require_setup_phase("add_window");
        add_window_(name, false, static_cast<uint64_t>(std::max(lo, 0)),
                    static_cast<uint64_t>(std::max(hi, 0)), 0);
    }

    /// A macro-time phase window (µs-ALEX): photons with (macro mod period) in
    /// [lo, hi), all in macro-time ticks.
    void add_phase_window(const std::string& name, uint64_t period, uint64_t lo, uint64_t hi) {
        require_setup_phase("add_phase_window");
        if (period == 0) throw std::invalid_argument("StreamingBurstAnalyzer: period must be > 0");
        add_window_(name, true, lo, hi, period);
    }

    /// The burst rule: at least `min_photons` photons; m photons within T [s].
    void set_search(int min_photons, int window_photons, double window_time) {
        require_setup_phase("set_search");
        if (window_photons < 1)
            throw std::invalid_argument("StreamingBurstAnalyzer: window_photons must be >= 1");
        if (!(window_time > 0.0))
            throw std::invalid_argument("StreamingBurstAnalyzer: window_time must be > 0");
        L_ = min_photons; m_ = window_photons; T_ = window_time;
        const double t = T_ / dt_;
        ticks_ = t >= 0.0 ? static_cast<uint64_t>(t) : 0;
    }

    /// "all_photons" (the default) or "dcbs" (not implemented yet: raises).
    void set_search_mode(const std::string& mode) {
        require_setup_phase("set_search_mode");
        if (mode == "all_photons" || mode == "aps" || mode == "sliding_window") return;
        if (mode == "dcbs")
            throw std::invalid_argument(
                "StreamingBurstAnalyzer: dual-channel burst search (DCBS) is not implemented yet; "
                "use the all-photon search, optionally on chosen channels (set_search_channels)");
        throw std::invalid_argument("StreamingBurstAnalyzer: unknown search mode '" + mode + "'");
    }

    /// The routing channels the search runs on. Default (empty): every detector
    /// channel, or every photon when no detector is declared.
    void set_search_channels(const std::vector<int>& channels) {
        require_setup_phase("set_search_channels");
        for (int c : channels) check_channel(c);
        search_channels_ = channels;
    }

    // ---- setup: quantities --------------------------------------------------------

    /// sum(numerator streams) / sum(denominator streams), 0 where the
    /// denominator is 0; with `subtract_background` each stream contributes its
    /// counts minus rate x burst duration.
    void add_ratio(const std::string& name, const std::vector<std::string>& numerator,
                   const std::vector<std::string>& denominator, bool subtract_background) {
        require_setup_phase("add_ratio");
        Quantity q; q.kind = Q_RATIO; q.names = {name}; q.bg = subtract_background;
        for (const auto& s : numerator) q.a.push_back(stream_id(s));
        for (const auto& s : denominator) q.b.push_back(stream_id(s));
        quantities_.push_back(q);
    }

    /// Accurate FRET (Hellenkamp 2018), `corrected_es` with a per-burst
    /// background (rate x duration): E into `name_e`, S into `name_s` (S only
    /// when `aa` is given).
    void add_corrected_fret(const std::string& name_e, const std::string& name_s,
                            const std::string& dd, const std::string& da, const std::string& aa,
                            double gamma, double beta, double alpha, double delta,
                            bool subtract_background) {
        require_setup_phase("add_corrected_fret");
        if (!(beta != 0.0)) throw std::invalid_argument("StreamingBurstAnalyzer: beta must be non-zero");
        Quantity q; q.kind = Q_FRET; q.bg = subtract_background;
        q.names = {name_e};
        q.a = {stream_id(dd), stream_id(da)};
        if (!aa.empty()) { q.a.push_back(stream_id(aa)); q.names.push_back(name_s); }
        q.p = {gamma, beta, alpha, delta};
        quantities_.push_back(q);
    }

    /// Steady-state anisotropy (DecayFit::r / rs):
    /// r = (Fp - g Fs) / (Fp (1 - 3 l2) + (2 - 3 l1) g Fs), 0 where the
    /// denominator is 0.
    void add_anisotropy(const std::string& name, const std::string& parallel,
                        const std::string& perpendicular, double g, double l1, double l2,
                        bool subtract_background) {
        require_setup_phase("add_anisotropy");
        Quantity q; q.kind = Q_ANISO; q.names = {name}; q.bg = subtract_background;
        q.a = {stream_id(parallel), stream_id(perpendicular)};
        q.p = {g, l1, l2};
        quantities_.push_back(q);
    }

    /// Mean micro time of a stream's burst photons, in ns (sentinel -1 for none).
    void add_mean_microtime(const std::string& name, const std::string& stream) {
        require_setup_phase("add_mean_microtime");
        Quantity q; q.kind = Q_MICRO; q.names = {name}; q.a = {stream_id(stream)};
        quantities_.push_back(q);
    }

    /// Fix a stream's background rate (counts / s) instead of the live estimate;
    /// a negative rate returns to the live estimate.
    void set_background_rate(const std::string& stream, double rate_cps) {
        const int s = stream_id(stream);
        if (fixed_bg_.size() < n_streams()) fixed_bg_.assign(n_streams(), -1.0);
        fixed_bg_[static_cast<size_t>(s)] = rate_cps;
    }

    // ---- setup: accumulators (any time; a late histogram is filled from the table) ----

    /// A histogram of table column `x` (1-D) or of `x` against `y` (2-D, `y`
    /// non-empty), half-open bins. Returns its index.
    int add_histogram(const std::string& x, int n_x, double x_min, double x_max,
                      const std::string& y, int n_y, double y_min, double y_max) {
        if (n_x < 1 || !(x_max > x_min) || (!y.empty() && (n_y < 1 || !(y_max > y_min))))
            throw std::invalid_argument("StreamingBurstAnalyzer: bad histogram geometry");
        Histogram h;
        h.x = x; h.nx = n_x; h.x0 = x_min; h.x1 = x_max;
        h.y = y; h.ny = y.empty() ? 1 : n_y; h.y0 = y_min; h.y1 = y_max;
        h.counts.assign(static_cast<size_t>(h.nx) * static_cast<size_t>(h.ny), 0.0);
        histograms_.push_back(h);
        if (started_) { bind(histograms_.back()); for (const auto& r : rows_) fill(histograms_.back(), r); }
        revision_++;
        return static_cast<int>(histograms_.size()) - 1;
    }

    /// A per-stream count-rate trace: bins of `bin_width` [s], aligned to macro
    /// time 0, the newest `max_bins` kept.
    void set_trace(double bin_width, int max_bins) {
        require_setup_phase("set_trace");
        if (!(bin_width > 0.0) || max_bins < 1)
            throw std::invalid_argument("StreamingBurstAnalyzer: bad trace geometry");
        trace_width_ = bin_width; trace_max_ = max_bins;
    }

    /// Per-stream micro-time decays of `n_bins` bins over micro times
    /// [0, micro_range): of all photons, of burst photons, and, with
    /// `per_burst`, one decay per burst and stream (the lifetime hook).
    void set_decays(int n_bins, int micro_range, bool per_burst) {
        require_setup_phase("set_decays");
        if (n_bins < 1 || micro_range < 1)
            throw std::invalid_argument("StreamingBurstAnalyzer: bad decay geometry");
        decay_bins_ = n_bins; decay_range_ = micro_range; per_burst_decays_ = per_burst;
    }

    // ---- photons ------------------------------------------------------------------

    /// Feed one photon. Macro times must not decrease.
    /// @return true iff this photon closed a burst that was recorded.
    bool push_photon(uint64_t macro_time, uint16_t micro_time, int channel) {
        if (!started_) start();
        const bool is_search = channel >= 0 && channel < 256 && search_[channel];
        const bool is_det = channel >= 0 && channel < 256 && detector_ch_[channel];
        if (!is_search && !is_det) return false;

        Photon p;
        p.macro = macro_time; p.micro = micro_time;
        p.dmask = detector_mask(micro_time, channel);
        p.wmask = window_mask(macro_time, micro_time);
        p.index = n_accepted_++;
        if (p.index == 0) first_macro_ = macro_time;
        last_macro_ = macro_time;
        observe(p);

        bool closed = false;
        if (is_search) {
            closed = push_search(p);
        } else {
            buffer_.push_back(p);
        }
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

    /// Forget every photon and burst; the setup is kept (and reopens for edits).
    void clear() {
        started_ = false;
        in_burst_ = false;
        n_accepted_ = n_search_ = 0;
        first_macro_ = last_macro_ = 0;
        rows_.clear();
        buffer_.clear();
        burst_decays_.clear();
        for (auto& h : histograms_) std::fill(h.counts.begin(), h.counts.end(), 0.0);
        revision_++;
    }

    // ---- live queries -------------------------------------------------------------

    size_t burst_count() const { return rows_.size(); }
    size_t photon_count() const { return static_cast<size_t>(n_accepted_); }
    uint64_t revision() const { return revision_; }
    double elapsed_seconds() const {
        return n_accepted_ ? static_cast<double>(last_macro_ - first_macro_) * dt_ : 0.0;
    }
    double burst_rate_hz() const {
        const double t = elapsed_seconds();
        return t > 0.0 ? static_cast<double>(rows_.size()) / t : 0.0;
    }

    /// Streams in index order: every "<window> <detector>", then every "<detector>".
    std::vector<std::string> stream_names() const {
        std::vector<std::string> n;
        for (const auto& w : windows_) for (const auto& d : detectors_) n.push_back(w.name + " " + d.name);
        for (const auto& d : detectors_) n.push_back(d.name);
        return n;
    }
    /// Live (or fixed) background rate of a stream, counts / s.
    double background_rate(const std::string& stream) const {
        return stream_bg(stream_id(stream)) / dt_;
    }
    /// Photons of a stream so far (all photons, not only bursts).
    double stream_photons(const std::string& stream) const {
        const int s = stream_id(stream);
        return started_ ? static_cast<double>(total_[static_cast<size_t>(s)]) : 0.0;
    }

    int histogram_count() const { return static_cast<int>(histograms_.size()); }
    std::string histogram_x(int i) const { return hist(i).x; }
    std::string histogram_y(int i) const { return hist(i).y; }
    int histogram_nx(int i) const { return hist(i).nx; }
    int histogram_ny(int i) const { return hist(i).y.empty() ? 0 : hist(i).ny; }
    std::vector<double> histogram_x_edges(int i) const { return edges(hist(i).nx, hist(i).x0, hist(i).x1); }
    std::vector<double> histogram_y_edges(int i) const {
        return hist(i).y.empty() ? std::vector<double>() : edges(hist(i).ny, hist(i).y0, hist(i).y1);
    }
    /// Row-major (nx, ny) for a 2-D histogram, length nx for a 1-D one.
    const std::vector<double>& histogram(int i) const { return hist(i).counts; }

    double trace_bin_width() const { return trace_width_; }
    /// Absolute index (macro time 0 = bin 0) of the first retained trace bin.
    int64_t trace_first_bin() const { return trace_first_; }
    std::vector<double> trace(const std::string& stream) const {
        const int s = stream_id(stream);
        std::vector<double> out;
        if (!started_ || trace_width_ <= 0.0) return out;
        out.reserve(trace_.size());
        for (const auto& bin : trace_) out.push_back(bin[static_cast<size_t>(s)]);
        return out;
    }
    int decay_bins() const { return decay_bins_; }
    int decay_range() const { return decay_range_; }
    /// Micro-time decay of a stream: every photon, or burst photons only.
    std::vector<double> decay(const std::string& stream, bool bursts_only) const {
        const int s = stream_id(stream);
        if (!started_ || decay_bins_ == 0) return {};
        const auto& src = bursts_only ? decay_burst_ : decay_all_;
        const size_t nb = static_cast<size_t>(decay_bins_);
        return std::vector<double>(src.begin() + static_cast<long>(static_cast<size_t>(s) * nb),
                                   src.begin() + static_cast<long>((static_cast<size_t>(s) + 1) * nb));
    }
    /// The decay of one recorded burst in one stream (needs per_burst decays).
    std::vector<double> burst_decay(int burst, const std::string& stream) const {
        const int s = stream_id(stream);
        if (!per_burst_decays_ || burst < 0 || static_cast<size_t>(burst) >= burst_decays_.size())
            throw std::out_of_range("StreamingBurstAnalyzer: no such burst decay (set_decays(..., true))");
        const size_t nb = static_cast<size_t>(decay_bins_);
        const auto& d = burst_decays_[static_cast<size_t>(burst)];
        return std::vector<double>(d.begin() + static_cast<long>(static_cast<size_t>(s) * nb),
                                   d.begin() + static_cast<long>((static_cast<size_t>(s) + 1) * nb));
    }

    // ---- the burst table ------------------------------------------------------------

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
        for (const auto& q : quantities_) for (const auto& name : q.names) n.push_back(name);
        return n;
    }

    std::vector<double> table_column(const std::string& name) const {
        const int c = column_index(name);
        if (c < 0) throw std::invalid_argument("StreamingBurstAnalyzer: no column '" + name + "'");
        std::vector<double> out(rows_.size());
        for (size_t r = 0; r < rows_.size(); ++r) out[r] = rows_[r][static_cast<size_t>(c)];
        return out;
    }

    int min_photons() const { return L_; }
    int window_photons() const { return m_; }
    double window_time() const { return T_; }
    double macro_time_resolution() const { return dt_; }
    double micro_time_resolution() const { return micro_dt_; }

private:
    struct Detector { std::string name; std::vector<int> channels; std::vector<std::pair<int, int>> ranges; };
    struct Window { std::string name; bool phase; uint64_t lo, hi, period; };
    struct Photon { uint64_t macro = 0; uint16_t micro = 0; uint32_t dmask = 0, wmask = 0; uint64_t index = 0; };
    enum QKind { Q_RATIO, Q_FRET, Q_ANISO, Q_MICRO };
    struct Quantity { QKind kind; std::vector<std::string> names; std::vector<int> a, b; std::vector<double> p; bool bg = false; };
    struct Histogram {
        std::string x, y; int nx, ny; double x0, x1, y0, y1;
        int cx = -1, cy = -1; std::vector<double> counts;
    };

    // configuration
    double dt_, micro_dt_;
    int L_ = 20, m_ = 10;
    double T_ = 5e-4;
    uint64_t ticks_ = 0;
    std::vector<Detector> detectors_;
    std::vector<Window> windows_;
    std::vector<int> search_channels_;
    std::vector<Quantity> quantities_;
    std::vector<double> fixed_bg_;
    std::vector<Histogram> histograms_;
    double trace_width_ = 0.0; int trace_max_ = 0;
    int decay_bins_ = 0, decay_range_ = 0; bool per_burst_decays_ = false;

    // derived at start
    bool started_ = false;
    bool search_[256] = {false};
    bool detector_ch_[256] = {false};
    uint64_t trace_ticks_ = 0;

    // state
    std::vector<Photon> ring_;                 // last m search photons
    std::deque<Photon> buffer_;                // non-search detector photons not yet past
    uint64_t n_accepted_ = 0, n_search_ = 0, first_macro_ = 0, last_macro_ = 0;
    bool in_burst_ = false;
    std::vector<int64_t> total_, in_bursts_;   // per stream
    uint64_t burst_ticks_ = 0;
    std::deque<std::vector<double>> trace_;
    int64_t trace_first_ = 0;
    std::vector<double> decay_all_, decay_burst_;
    std::vector<std::vector<double>> burst_decays_;
    // the open burst
    uint64_t b_t0_ = 0, b_t1_ = 0, b_i0_ = 0, b_i1_ = 0;
    int64_t b_search_n_ = 0;
    std::vector<int64_t> b_n_;                 // per stream
    std::vector<uint64_t> b_s_t0_, b_s_t1_, b_s_i0_, b_s_i1_;
    std::vector<double> b_s_micro_;
    std::vector<double> b_decay_;
    // results
    std::vector<std::vector<double>> rows_;
    uint64_t revision_ = 0;

    static constexpr double kSentinel = -1.0;

    void require_setup_phase(const char* what) const {
        if (started_)
            throw std::logic_error(std::string("StreamingBurstAnalyzer: ") + what +
                                   " after the first photon; call clear() first");
    }
    static void check_channel(int c) {
        if (c < 0 || c > 255) throw std::invalid_argument("StreamingBurstAnalyzer: channel out of range 0..255");
    }
    void add_window_(const std::string& name, bool phase, uint64_t lo, uint64_t hi, uint64_t period) {
        if (windows_.size() >= 32) throw std::invalid_argument("StreamingBurstAnalyzer: at most 32 windows");
        windows_.push_back({name, phase, lo, hi, period});
    }
    size_t n_streams() const { return windows_.size() * detectors_.size() + detectors_.size(); }
    int stream_id(const std::string& name) const {
        const size_t nd = detectors_.size();
        for (size_t w = 0; w < windows_.size(); ++w)
            for (size_t d = 0; d < nd; ++d)
                if (windows_[w].name + " " + detectors_[d].name == name) return static_cast<int>(w * nd + d);
        for (size_t d = 0; d < nd; ++d)
            if (detectors_[d].name == name) return static_cast<int>(windows_.size() * nd + d);
        throw std::invalid_argument("StreamingBurstAnalyzer: no stream '" + name +
                                    "' (streams are '<window> <detector>' and '<detector>')");
    }
    int column_index(const std::string& name) const {
        const auto names = table_names();
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return static_cast<int>(i);
        return -1;
    }
    const Histogram& hist(int i) const {
        if (i < 0 || static_cast<size_t>(i) >= histograms_.size())
            throw std::out_of_range("StreamingBurstAnalyzer: no such histogram");
        return histograms_[static_cast<size_t>(i)];
    }
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

    void bind(Histogram& h) {
        h.cx = column_index(h.x);
        if (h.cx < 0) throw std::invalid_argument("StreamingBurstAnalyzer: no column '" + h.x + "' to histogram");
        h.cy = -1;
        if (!h.y.empty()) {
            h.cy = column_index(h.y);
            if (h.cy < 0) throw std::invalid_argument("StreamingBurstAnalyzer: no column '" + h.y + "' to histogram");
        }
    }

    void start() {
        // validate every histogram's columns now that the table is fixed
        for (auto& h : histograms_) bind(h);
        std::fill(std::begin(search_), std::end(search_), false);
        std::fill(std::begin(detector_ch_), std::end(detector_ch_), false);
        for (const auto& d : detectors_) for (int c : d.channels) detector_ch_[c] = true;
        if (!search_channels_.empty()) {
            for (int c : search_channels_) search_[c] = true;
        } else if (!detectors_.empty()) {
            for (int c = 0; c < 256; ++c) search_[c] = detector_ch_[c];
        } else {
            std::fill(std::begin(search_), std::end(search_), true);
        }
        const size_t ns = n_streams();
        ring_.assign(static_cast<size_t>(m_), Photon());
        total_.assign(ns, 0); in_bursts_.assign(ns, 0);
        if (fixed_bg_.size() < ns) fixed_bg_.resize(ns, -1.0);
        burst_ticks_ = 0;
        b_n_.assign(ns, 0); b_s_t0_.assign(ns, 0); b_s_t1_.assign(ns, 0);
        b_s_i0_.assign(ns, 0); b_s_i1_.assign(ns, 0); b_s_micro_.assign(ns, 0.0);
        trace_.clear(); trace_first_ = 0;
        trace_ticks_ = trace_width_ > 0.0 ? std::max<uint64_t>(1, static_cast<uint64_t>(trace_width_ / dt_)) : 0;
        const size_t db = static_cast<size_t>(decay_bins_);
        decay_all_.assign(ns * db, 0.0); decay_burst_.assign(ns * db, 0.0);
        b_decay_.assign(ns * db, 0.0);
        started_ = true;
        revision_++;
    }

    uint32_t detector_mask(uint16_t micro, int channel) const {
        uint32_t m = 0;
        for (size_t d = 0; d < detectors_.size(); ++d) {
            const Detector& det = detectors_[d];
            if (std::find(det.channels.begin(), det.channels.end(), channel) == det.channels.end()) continue;
            bool in = det.ranges.empty();
            for (const auto& r : det.ranges)
                if (micro >= r.first && micro < r.second) { in = true; break; }
            if (in) m |= (1u << d);
        }
        return m;
    }
    uint32_t window_mask(uint64_t macro, uint16_t micro) const {
        uint32_t m = 0;
        for (size_t w = 0; w < windows_.size(); ++w) {
            const Window& win = windows_[w];
            const uint64_t v = win.phase ? macro % win.period : static_cast<uint64_t>(micro);
            if (v >= win.lo && v < win.hi) m |= (1u << w);
        }
        return m;
    }
    // Calls f(stream) for every stream the photon belongs to.
    template <class F> void for_streams(const Photon& p, F f) const {
        const size_t nd = detectors_.size();
        for (size_t d = 0; d < nd; ++d) {
            if (!(p.dmask & (1u << d))) continue;
            for (size_t w = 0; w < windows_.size(); ++w)
                if (p.wmask & (1u << w)) f(w * nd + d);
            f(windows_.size() * nd + d);
        }
    }
    int decay_bin(uint16_t micro) const {
        if (decay_bins_ == 0 || micro >= decay_range_) return -1;
        return static_cast<int>(static_cast<int64_t>(micro) * decay_bins_ / decay_range_);
    }

    // every accepted photon: stream totals, trace, decays
    void observe(const Photon& p) {
        const int db = decay_bin(p.micro);
        if (trace_ticks_) {
            const int64_t bin = static_cast<int64_t>(p.macro / trace_ticks_);
            if (trace_.empty()) trace_first_ = bin;
            while (trace_first_ + static_cast<int64_t>(trace_.size()) <= bin)
                trace_.push_back(std::vector<double>(n_streams(), 0.0));
            while (static_cast<int>(trace_.size()) > trace_max_) { trace_.pop_front(); trace_first_++; }
            const int64_t k = bin - trace_first_;
            if (k >= 0) {
                auto& slot = trace_[static_cast<size_t>(k)];
                for_streams(p, [&](size_t s) { slot[s] += 1.0; });
            }
        }
        for_streams(p, [&](size_t s) {
            total_[s]++;
            if (db >= 0) decay_all_[s * static_cast<size_t>(decay_bins_) + static_cast<size_t>(db)] += 1.0;
        });
    }

    bool push_search(const Photon& p) {
        const uint64_t j = n_search_;
        ring_[static_cast<size_t>(j % static_cast<uint64_t>(m_))] = p;
        n_search_++;
        if (n_search_ < static_cast<uint64_t>(m_)) return false;
        // oldest search photon of the m-photon window ending here
        const Photon& back = ring_[static_cast<size_t>(n_search_ % static_cast<uint64_t>(m_))];
        const bool in_window = (p.macro - back.macro) <= ticks_;
        bool closed = false;
        if (in_window && !in_burst_) {
            in_burst_ = true;
            open_burst();
            for (uint64_t k = j - static_cast<uint64_t>(m_) + 1; k <= j; ++k)
                add_search(ring_[static_cast<size_t>(k % static_cast<uint64_t>(m_))]);
        } else if (in_window && in_burst_) {
            add_search(p);
        } else if (!in_window && in_burst_) {
            in_burst_ = false;
            closed = close_burst();
        }
        prune();
        return closed;
    }

    // Non-search photons can still fall in a burst that opens later, as far
    // back as the oldest search photon of the current window (or the open
    // burst's start); older ones are dropped.
    void prune() {
        if (buffer_.empty()) return;
        uint64_t keep_from;
        if (in_burst_) keep_from = b_t0_;
        else if (n_search_ >= static_cast<uint64_t>(m_))
            keep_from = ring_[static_cast<size_t>(n_search_ % static_cast<uint64_t>(m_))].macro;
        else keep_from = 0;
        // the next window starts one search photon later; its oldest photon is
        // not older than the current oldest, so `keep_from` is safe
        while (!buffer_.empty() && buffer_.front().macro < keep_from) buffer_.pop_front();
    }

    void open_burst() {
        b_search_n_ = 0;
        std::fill(b_n_.begin(), b_n_.end(), 0);
        std::fill(b_s_micro_.begin(), b_s_micro_.end(), 0.0);
        std::fill(b_decay_.begin(), b_decay_.end(), 0.0);
    }
    void add_photon_to_burst(const Photon& p) {
        const int db = decay_bin(p.micro);
        for_streams(p, [&](size_t s) {
            if (b_n_[s] == 0) { b_s_t0_[s] = p.macro; b_s_i0_[s] = p.index; }
            b_s_t1_[s] = p.macro; b_s_i1_[s] = p.index;
            b_n_[s]++;
            b_s_micro_[s] += p.micro;
            if (db >= 0)
                b_decay_[s * static_cast<size_t>(decay_bins_) + static_cast<size_t>(db)] += 1.0;
        });
    }
    void add_search(const Photon& p) {
        if (b_search_n_ == 0) { b_t0_ = p.macro; b_i0_ = p.index; }
        b_t1_ = p.macro; b_i1_ = p.index;
        b_search_n_++;
        add_photon_to_burst(p);
    }

    double stream_bg(int s) const {   // photons per tick
        if (s < 0 || !started_) return 0.0;
        const size_t k = static_cast<size_t>(s);
        if (k < fixed_bg_.size() && fixed_bg_[k] >= 0.0) return fixed_bg_[k] * dt_;
        if (n_accepted_ == 0) return 0.0;
        const uint64_t span = last_macro_ - first_macro_;
        if (span <= burst_ticks_) return 0.0;
        const double n = static_cast<double>(total_[k] - in_bursts_[k]);
        return n > 0.0 ? n / static_cast<double>(span - burst_ticks_) : 0.0;
    }

    bool close_burst() {
        // photons of detectors outside the search that fall within the burst
        std::vector<Photon> extra;
        for (const auto& p : buffer_)
            if (p.macro >= b_t0_ && p.macro <= b_t1_) extra.push_back(p);
        if (b_search_n_ < L_ || b_search_n_ <= 0) return false;
        for (const auto& p : extra) add_photon_to_burst(p);

        const size_t nd = detectors_.size(), nw = windows_.size(), ns = n_streams();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double ms = dt_ * 1e3;
        const double micro_ns = micro_dt_ > 0.0 ? micro_dt_ * 1e9 : 0.0;
        const int64_t count = b_search_n_ + static_cast<int64_t>(extra.size());

        std::vector<double> v;
        const double dur = static_cast<double>(b_t1_ - b_t0_) * ms;
        v.push_back(static_cast<double>(b_i0_));
        v.push_back(static_cast<double>(b_i1_));
        v.push_back(dur);
        v.push_back((static_cast<double>(b_t1_) + static_cast<double>(b_t0_)) / 2.0 * ms);
        v.push_back(static_cast<double>(count));
        v.push_back(dur > 0.0 ? static_cast<double>(count) / dur : nan);
        for (size_t d = 0; d < nd; ++d) {
            const size_t s = nw * nd + d;
            if (b_n_[s] == 0) { v.insert(v.end(), {-1.0, -1.0, kSentinel, kSentinel, 0.0, kSentinel}); continue; }
            const double d_ms = static_cast<double>(b_s_t1_[s] - b_s_t0_[s]) * ms;
            v.push_back(static_cast<double>(b_s_i0_[s]));
            v.push_back(static_cast<double>(b_s_i1_[s]));
            v.push_back(d_ms);
            v.push_back((static_cast<double>(b_s_t1_[s]) + static_cast<double>(b_s_t0_[s])) / 2.0 * ms);
            v.push_back(static_cast<double>(b_n_[s]));
            v.push_back(d_ms > 0.0 ? static_cast<double>(b_n_[s]) / d_ms : nan);
        }
        for (size_t w = 0; w < nw; ++w)
            for (size_t d = 0; d < nd; ++d) {
                const size_t s = w * nd + d;
                if (b_n_[s] == 0) { v.insert(v.end(), {kSentinel, 0.0, kSentinel}); continue; }
                const double d_ms = static_cast<double>(b_s_t1_[s] - b_s_t0_[s]) * ms;
                v.push_back(d_ms > 0.0 ? static_cast<double>(b_n_[s]) / d_ms : nan);
                v.push_back(static_cast<double>(b_n_[s]));
                v.push_back(d_ms);
            }
        for (size_t d = 0; d < nd; ++d) {
            const size_t s = nw * nd + d;
            v.push_back(b_n_[s] > 0 && micro_ns > 0.0
                        ? b_s_micro_[s] / static_cast<double>(b_n_[s]) * micro_ns : kSentinel);
        }

        // this burst no longer counts as background, before the quantities use it
        // (the estimate is of the time *between* bursts)
        for (size_t s = 0; s < ns; ++s) in_bursts_[s] += b_n_[s];
        burst_ticks_ += b_t1_ - b_t0_;

        const double span = static_cast<double>(b_t1_ - b_t0_);
        const auto counts = [&](int s, bool bg) {
            const double n = static_cast<double>(b_n_[static_cast<size_t>(s)]);
            return bg ? n - stream_bg(s) * span : n;
        };
        for (const auto& q : quantities_) {
            switch (q.kind) {
                case Q_RATIO: {
                    double num = 0.0, den = 0.0;
                    for (int s : q.a) num += counts(s, q.bg);
                    for (int s : q.b) den += counts(s, q.bg);
                    v.push_back(den != 0.0 ? num / den : 0.0);
                    break;
                }
                case Q_FRET: {
                    const double g = q.p[0], be = q.p[1], al = q.p[2], de = q.p[3];
                    const bool has_s = q.a.size() > 2;
                    const double f_dd = counts(q.a[0], q.bg);
                    const double f_aa = has_s ? counts(q.a[2], q.bg) : 0.0;
                    const double f_da = counts(q.a[1], q.bg) - al * f_dd - de * f_aa;
                    const double e_den = f_da + g * f_dd;
                    v.push_back(e_den > 0.0 ? f_da / e_den : 0.0);
                    if (has_s) {
                        const double s_den = g * f_dd + f_da + f_aa / be;
                        v.push_back(s_den != 0.0 ? (g * f_dd + f_da) / s_den : 0.0);
                    }
                    break;
                }
                case Q_ANISO: {
                    const double fp = counts(q.a[0], q.bg), fs = counts(q.a[1], q.bg);
                    const double g = q.p[0], l1 = q.p[1], l2 = q.p[2];
                    const double den = fp * (1.0 - 3.0 * l2) + (2.0 - 3.0 * l1) * g * fs;
                    v.push_back(den != 0.0 ? (fp - g * fs) / den : 0.0);
                    break;
                }
                case Q_MICRO: {
                    const size_t s = static_cast<size_t>(q.a[0]);
                    v.push_back(b_n_[s] > 0 && micro_ns > 0.0
                                ? b_s_micro_[s] / static_cast<double>(b_n_[s]) * micro_ns : kSentinel);
                    break;
                }
            }
        }

        if (decay_bins_ > 0) {
            for (size_t i = 0; i < b_decay_.size(); ++i) decay_burst_[i] += b_decay_[i];
            if (per_burst_decays_) burst_decays_.push_back(b_decay_);
        }
        rows_.push_back(std::move(v));
        for (auto& h : histograms_) fill(h, rows_.back());
        revision_++;
        return true;
    }

    // The bin is the one whose reported edges hold x (edges as `edges()` makes
    // them), so a value on an edge lands where numpy's searchsorted puts it.
    static int bin_of(double x, int n, double lo, double hi) {
        if (!(x >= lo) || !(x < hi)) return -1;
        int i = static_cast<int>((x - lo) / (hi - lo) * n);
        if (i >= n) i = n - 1;
        const auto edge = [&](int k) { return lo + (hi - lo) * k / n; };
        while (i > 0 && x < edge(i)) --i;
        while (i < n - 1 && x >= edge(i + 1)) ++i;
        return i;
    }
    static void fill(Histogram& h, const std::vector<double>& row) {
        const int ix = bin_of(row[static_cast<size_t>(h.cx)], h.nx, h.x0, h.x1);
        if (ix < 0) return;
        int iy = 0;
        if (h.cy >= 0) {
            iy = bin_of(row[static_cast<size_t>(h.cy)], h.ny, h.y0, h.y1);
            if (iy < 0) return;
        }
        h.counts[static_cast<size_t>(ix) * static_cast<size_t>(h.ny) + static_cast<size_t>(iy)] += 1.0;
    }
};

} // namespace tttrlib

#endif // TTTRLIB_STREAMING_BURST_ANALYZER_H
