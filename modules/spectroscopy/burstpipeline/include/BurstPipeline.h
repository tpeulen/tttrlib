// SPDX-License-Identifier: BSD-3-Clause
/*!
 * \file BurstPipeline.h
 * \brief One native pass from a photon stream to a burst table, per-burst decays and fits.
 *
 * A smFRET burst analysis used to cross from Python into the library between every
 * stage: the photon arrays were copied out for each filter, a boolean mask per
 * detector was built for the summary, the burst table was converted into rows, the
 * per-burst decays were histogrammed into a dense matrix and that matrix was turned
 * into a list of Python floats on its way into the fit. `BurstPipeline` does the
 * same work in three calls per measurement and keeps every result in buffers it
 * owns, which a binding exposes without copying.
 *
 * - `run(tttr)` makes one chunked pass over the records. Each photon is classified
 *   once, through a (routing channel, micro time) look-up table, into the detectors,
 *   the excitation windows and the polarisation role it belongs to. The burst
 *   search pulls the photons of its detector subset through the same cursor; the
 *   selection mask (search result, delta-macro-time filter, gap filling, minimum
 *   size) is evaluated as runs, never as an array; and every finished burst gets its
 *   feature row and one sparse (CSR) decay row per decay group.
 * - `strict_background(...)` is the background-photon selection of ChiSurf's
 *   `strict_background_mask`, computed as streaming passes. It returns the
 *   statistics and the micro-time histograms of the kept photons; the mask itself is
 *   never built.
 * - `fit(result, config)` runs decay fits on the CSR decays in place, one burst row
 *   at a time per worker thread, so the dense decay matrix never exists.
 *
 * Every call takes its whole configuration at once, as a JSON document of plain
 * values (`BurstPipeline(config)`, `fit(..., config)`), and never calls back into the
 * caller. Progress and cancellation are two atomics the caller reads and sets from
 * its own thread (`progress_*`, `cancel`).
 *
 * The configuration and every output column are documented in the module README
 * (`modules/spectroscopy/burstpipeline/README.md`).
 */
#ifndef TTTRLIB_BURSTPIPELINE_H
#define TTTRLIB_BURSTPIPELINE_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "TTTR.h"

namespace tttrlib {

/*!
 * \brief Columns produced by a `BurstPipeline` run, owned here.
 *
 * Scalar columns are addressed by name (`column_names()`), stored as `float64` or
 * `int64`. The decays are one CSR matrix per decay group: row `b` is burst `b`, the
 * column index is `role * n_bins + bin` (role 0 parallel, 1 perpendicular), and the
 * value is the photon count. `decay_sum(g)` is every burst's decay added, `2 *
 * n_bins` long. The buffers do not move after the call that filled them returns, so
 * a view stays valid for the lifetime of this object.
 */
class BurstPipelineResult {
public:
    /*! Number of bursts (rows of every burst column). */
    long long n_bursts() const { return n_bursts_; }

    /*! Names of every scalar column, float64 ones first. */
    std::vector<std::string> column_names() const;

    /*! Whether a float64 column of that name exists. */
    bool has_column(const std::string &name) const;

    /*! Whether an int64 column of that name exists. */
    bool has_index_column(const std::string &name) const;

    /*!
     * \brief View of a float64 column.
     * \throws std::out_of_range when no float64 column has that name.
     */
    void column(const std::string &name, double **view, int *n);

    /*!
     * \brief View of an int64 column.
     * \throws std::out_of_range when no int64 column has that name.
     */
    void index_column(const std::string &name, long long **view, int *n);

    /*! Number of decay groups (one per distinct detector channel list). */
    int n_decay_groups() const { return static_cast<int>(groups_.size()); }

    /*! Name of decay group \p g: the first detector that uses its channel list. */
    std::string decay_group_name(int g) const;

    /*! Channels of decay group \p g, in the detector's order (P at even, S at odd positions). */
    std::vector<int> decay_group_channels(int g) const;

    /*! Index of the decay group of detector \p name, or -1. */
    int decay_group_of(const std::string &detector) const;

    /*! Bins per polarisation of decay group \p g. */
    int decay_n_bins(int g) const;

    /*! CSR row pointer of decay group \p g, `n_bursts + 1` long. */
    void decay_indptr(int g, long long **view, int *n);

    /*! CSR column index of decay group \p g: `role * n_bins + bin`. */
    void decay_bins(int g, int **view, int *n);

    /*! CSR values (photon counts) of decay group \p g. */
    void decay_counts(int g, int **view, int *n);

    /*! Summed decay of decay group \p g over every burst, `2 * n_bins` long. */
    void decay_sum(int g, long long **view, int *n);

    /*!
     * \brief Raw micro-time histogram of the photons outside every burst, per channel.
     *
     * One row per entry of `histogram_channels()`, `histogram_length()` bins of one raw
     * micro-time channel each, row-major.
     */
    void outside_histogram(long long **view, int *n);

    /*! Routing channels of the rows of `outside_histogram`. */
    std::vector<int> histogram_channels() const { return hist_channels_; }

    /*! Bins per row of `outside_histogram`. */
    int histogram_length() const { return hist_length_; }

    /*!
     * \brief Timing, sizes and counters of the run, as JSON.
     *
     * Keys: `photons`, `subset_photons`, `bursts`, `search_bursts`, `cancelled`,
     * `seconds` (per stage), `peak_buffered_photons` (the largest number of photon
     * codes held at once), `peak_buffered_subset`, `bytes_out` (bytes of every buffer
     * this result owns).
     */
    std::string stats_json() const { return stats_json_; }

    /*! True when the run was stopped by `BurstPipeline::cancel()`. */
    bool cancelled() const { return cancelled_; }

#ifndef SWIG
    // ---- C++ side: the pipeline fills these ----------------------------------------
    struct DecayGroup {
        std::string name;
        std::vector<int> channels;
        bool mirror = false;
        int n_bins = 0;
        std::vector<long long> indptr{0};
        std::vector<int> bins;
        std::vector<int> counts;
        std::vector<long long> sum;
    };

    std::map<std::string, std::vector<double>> f64_;
    std::map<std::string, std::vector<long long>> i64_;
    std::vector<DecayGroup> groups_;
    std::vector<std::string> detector_names_;
    std::vector<int> detector_group_;
    std::vector<int> hist_channels_;
    int hist_length_ = 0;
    std::vector<long long> outside_hist_;
    long long n_bursts_ = 0;
    bool cancelled_ = false;
    std::string stats_json_ = "{}";
    double micro_ns_ = 0.0;
    double macro_res_ = 0.0;
    int binning_ = 1;

#endif  // SWIG

    /*! Bytes of every buffer this result holds. */
    long long nbytes() const;
};


/*!
 * \brief Result of the strict background-photon selection.
 *
 * The fields of ChiSurf's `StrictBackground` (seconds kept, total seconds, rate,
 * threshold, photons kept, iterations, rate scale), plus the raw micro-time
 * histogram of the kept photons per routing channel of `histogram_channels`.
 */
struct StrictBackgroundResult {
    double seconds = 0.0;
    double total_seconds = 0.0;
    double rate_khz = 0.0;
    double threshold_khz = 0.0;
    long long photons = 0;
    long long selected_photons = 0;
    int iterations = 0;
    double rate_scale = 1.0;
    /*! Gaps (between consecutive kept selected photons) behind the last rate. */
    long long inner_gaps = 0;
    /*! Bins per histogram row (raw micro-time channels). */
    int length = 0;
#ifndef SWIG
    std::vector<int> channels;
    std::vector<long long> histogram;
#endif

    /*! Routing channels of the histogram rows. */
    std::vector<int> histogram_channels() const { return channels; }

    /*! Kept-photon histogram view, `channels.size() * length`, row-major. */
    void histogram_view(long long **view, int *n) {
        *view = histogram.data();
        *n = static_cast<int>(histogram.size());
    }
};


/*!
 * \brief The strict background selection, kept so the caller can tighten it.
 *
 * `select()` runs the iteration of `strict_background_mask` (local-rate threshold
 * from the Poisson tail of the inter-photon gaps, until the rate converges). The
 * caller's acceptance test then looks at the returned histograms; `keep_at(threshold)`
 * recomputes the selection at a tightened threshold, and `gap_rate(fallback)` is the
 * rate from the gaps inside the current selection, or \p fallback when it has fewer
 * than 20. That is the whole of the original's accept loop, so the loop itself can
 * stay with the acceptance test.
 */
class StrictBackground {
public:
    /*!
     * \param tttr the measurement.
     * \param first first photon of every burst (inclusive).
     * \param last last photon of every burst (inclusive).
     * \param config JSON: `channels` (the selected channels, empty = all),
     *        `histogram_channels`, `margin_ms` (2.0), `margin_fraction` (0.5),
     *        `photon_window` (10), `false_alarm` (0.01), `iterations` (4),
     *        `rate_khz` (0 = estimate), `macro_time_resolution` (0 = header).
     */
    StrictBackground(std::shared_ptr<TTTR> tttr,
                     long long *first, int n_first,
                     long long *last, int n_last,
                     const std::string &config);

    /*! Run the iteration; returns the selection it settled on. */
    StrictBackgroundResult select();

    /*! The selection at local-rate threshold \p threshold_khz (kHz). */
    StrictBackgroundResult keep_at(double threshold_khz);

    /*! Rate (kHz) from the gaps inside the current selection, or \p fallback. */
    double gap_rate(double fallback) const;

    /*! `(m - 1) / Gamma(m - 1).quantile(false_alarm)`: rate to threshold factor. */
    double factor() const { return factor_; }

    /*! `rate_scale` of the current selection at \p rate_khz. */
    double rate_scale_at(double rate_khz) const;

private:
    std::shared_ptr<TTTR> tttr_;
    std::vector<double> flank_lo_, flank_hi_;
    std::vector<unsigned char> channel_sel_;  // 256 entries
    std::vector<int> hist_channels_;
    std::vector<int> hist_slot_;              // 256 entries, -1 = none
    int hist_length_ = 0;
    double res_ = 1.0;
    int m_ = 10;
    double factor_ = 1.0;
    int iterations_ = 4;
    double rate_khz_given_ = 0.0;
    double total_ = 0.0;
    double t0_ = 0.0, t1_ = 0.0;
    // the state of the current selection
    std::vector<long long> gap_hist_dense_;
    std::vector<unsigned long long> gap_overflow_;
    long long n_gaps_ = 0;
    double gap_sum_ = 0.0;
    double seconds_ = 0.0;
    long long kept_ = 0, kept_sel_ = 0;
    std::vector<long long> histogram_;

    void run_keep(double threshold_khz, bool histogram);
    double tail_rate_khz() const;
    StrictBackgroundResult snapshot(double rate, double threshold, int iterations) const;
};


/*!
 * \brief The fused burst pipeline; see the file header.
 */
class BurstPipeline {
public:
    /*!
     * \param config JSON with `detectors`, `windows`, `search`, `selection`,
     *        `decays` and optional `fits`, `chunk_size`, `n_threads`,
     *        `micro_time_resolution_ns`, `macro_time_resolution`. See the README.
     */
    explicit BurstPipeline(const std::string &config);

    /*! Search, select, summarise and histogram \p tttr; fits too when `fits` is set. */
    std::shared_ptr<BurstPipelineResult> run(std::shared_ptr<TTTR> tttr);

    /*!
     * \brief The decays of given bursts, without a search.
     *
     * For a caller that already has the bursts (a burst table read back): the same
     * classification and CSR decays as `run`, plus the static and detector features.
     * \p binning and \p n_bins replace the configured decay binning when positive.
     */
    std::shared_ptr<BurstPipelineResult> decays(std::shared_ptr<TTTR> tttr,
                                                long long *first, int n_first,
                                                long long *last, int n_last,
                                                int binning = 0, int n_bins = 0);

    /*!
     * \brief Fit the CSR decays of \p result in place and add the fit columns to it.
     *
     * \param config JSON `{"fits": [...], "n_threads": k}`, one entry per fitted
     *        detector (see the README). Columns written: `fit|<det>|ng_p`,
     *        `fit|<det>|ng_s`, `fit|<det>|reason` (int64), `fit|<det>|objective`,
     *        `fit|<det>|x|<i>`, `fit|<det>|r|<j>` (float64).
     */
    void fit(std::shared_ptr<BurstPipelineResult> result, const std::string &config);

    /*!
     * \brief One background pattern per burst for fit entry \p fit_index.
     *
     * Rows are bursts, columns are the fitted background (`2 * fitted bins`), already
     * normalised the way the model expects. Replaces the shared background of that
     * entry in the next `fit`.
     */
    void set_burst_backgrounds(int fit_index, double *backgrounds, int n_rows, int n_cols);

    /*! Ask a running call to stop at the next chunk. */
    void cancel() { cancel_.store(true); }

    /*! Photons the current or last call has read. */
    long long progress_photons() const { return photons_done_.load(); }

    /*! Photons of the measurement the current or last call reads. */
    long long progress_total() const { return photons_total_.load(); }

    /*! Bursts finished so far. */
    long long progress_bursts() const { return bursts_done_.load(); }

    /*! Bursts fitted so far (all fitted detectors of a burst count as one). */
    long long progress_fits() const { return fits_done_.load(); }

    /*! The configuration, as given. */
    std::string config() const { return config_; }

    /*! Number of calls into this object (`run`, `decays`, `fit`, ...), for crossing counts. */
    long long calls() const { return calls_.load(); }

#ifndef SWIG
    struct Impl;
#endif

private:
    std::string config_;
    std::shared_ptr<Impl> impl_;
    std::atomic<bool> cancel_{false};
    std::atomic<long long> photons_done_{0};
    std::atomic<long long> photons_total_{0};
    std::atomic<long long> bursts_done_{0};
    std::atomic<long long> fits_done_{0};
    std::atomic<long long> calls_{0};
};

}  // namespace tttrlib

#endif  // TTTRLIB_BURSTPIPELINE_H
