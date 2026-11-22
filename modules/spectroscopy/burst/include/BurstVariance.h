// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_BURSTVARIANCE_H
#define TTTRLIB_BURSTVARIANCE_H

// Validation: A/B-TESTED 2026-11-22 -- slicing, per-slice ratio, SD (ddof 0/1), pooled ratio and
//   binomial chi2 vs a NumPy transcription of the definitions below (photon slices; time windows
//   with and without period alignment; masks == streams) to 1e-12, and vs chisurf's NumPy engine
//   (core.fluorescence.burst.variance) to 1e-12 with identical slices. Static bound: coverage of
//   static photons at its quantile (0.90 +- 0.06 at q = 0.9), reproducible for a seed.
//   test/python/bva/test_burst_variance.py.
//   Register: okf/testing/algorithm-validation.md

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "BurstFeature.h"

namespace tttrlib {

/**
 * @brief Sub-burst variance of a two-stream photon ratio — the engine behind BVA and SVA.
 *
 * Burst variance analyses share one computation (BVA, SVA, and anisotropy variance with
 * ::set_statistic). Each burst is cut into slices, and for every
 * slice the ratio @f$ r = n_A / (n_A + n_B) @f$ of two photon streams is computed. The burst's
 * scatter of @f$ r @f$ over its slices is compared with the scatter a static molecule produces
 * from shot noise alone. A burst whose scatter is larger changed its ratio within the burst.
 *
 *  - **BVA** (Torella et al., Biophys. J. 2011): A = acceptor and B = donor photons under donor
 *    excitation. @f$ r @f$ is the proximity ratio, and excess scatter is FRET dynamics.
 *  - **SVA**: A = donor-excitation photons (both detectors) and B = acceptor-excitation photons.
 *    @f$ r @f$ is the stoichiometry, and excess scatter is bleaching, blinking or two molecules
 *    in the focus.
 *
 * Slicing works on the photons of A ∪ B inside the burst. Photons in neither stream are not
 * counted.
 *  - ::PHOTONS: consecutive chunks of ``photons_per_slice`` photons. The trailing partial chunk
 *    is dropped, so every slice has the same photon number.
 *  - ::TIME: abutting windows of ``window_ticks`` on the time axis (macro time, or the axis
 *    given to ::set_time_axis, e.g. folded µs-ALEX time or alternation-period indices). The
 *    windows are aligned to the burst's earliest photon, and the trailing partial window is
 *    dropped. With ``align_ticks > 0`` the first window starts at the first whole multiple of
 *    ``align_ticks`` after the earliest photon, and windows end at the last whole multiple before
 *    the latest photon. Under µs-ALEX (align = alternation period) every window then covers
 *    whole periods and samples both lasers equally.
 *
 * A slice counts when it holds at least ``min_photons`` photons. A burst is *tested* when it
 * has at least ``min_slices`` counted slices.
 *
 * Per tested burst: mean and SD (``ddof``) of the slice ratios, the pooled ratio
 * @f$ p = \sum n_A / \sum N @f$, the binomial @f$ \chi^2 = \sum (n_{A,i} - N_i p)^2 /
 * (N_i p (1-p)) @f$ (``k - 1`` degrees of freedom), the slice count and the mean slice photon
 * number. The photon number of every counted slice is kept (::get_slice_photons /
 * ::get_slice_owner) for the static bound.
 *
 * ::static_bound simulates each tested burst as a static molecule: binomial slices with the
 * burst's own photon numbers and pooled ratio, drawn from the counter-based tttrlib RNG, so the
 * result is the same for any thread count. The bound is the quantile of the simulated slice SD
 * times @f$ \sqrt{\phi} @f$, where @f$ \phi @f$ is the dispersion every molecule of a population
 * shares. ::classify turns observed SD and bound into 1 (static), 0 (dynamic) or -1 (untested).
 */
class BurstVariance : public BurstFeature {
public:
    /// The slice statistic.
    enum Statistic {
        RATIO = 0,       ///< n_A / (n_A + n_B)
        ANISOTROPY = 1   ///< (n_A - G n_B) / ((1 - 3 l2) n_A + (2 - 3 l1) G n_B), A parallel, B perpendicular
    };

    /// How bursts are cut into slices.
    enum SliceMode {
        PHOTONS = 0,  ///< chunks of a fixed number of photons
        TIME = 1      ///< fixed-length windows on the time axis
    };

    explicit BurstVariance(std::shared_ptr<TTTR> tttr) : BurstFeature(std::move(tttr)) {}
    explicit BurstVariance(std::shared_ptr<BurstFilter> burst_filter)
        : BurstFeature(std::move(burst_filter)) {}

    /// The numerator stream A of the ratio n_A / (n_A + n_B).
    void set_stream_a(
        const std::vector<int>& channels,
        const std::vector<std::pair<int, int>>& micro_time_ranges = {}
    ) { set_stream(STREAM_A, channels, micro_time_ranges); }

    /// The complement stream B of the ratio n_A / (n_A + n_B).
    void set_stream_b(
        const std::vector<int>& channels,
        const std::vector<std::pair<int, int>>& micro_time_ranges = {}
    ) { set_stream(STREAM_B, channels, micro_time_ranges); }

    /**
     * @brief Per-photon stream membership instead of channel/micro-time streams.
     *
     * One 0/1 entry per photon of the TTTR stream for A and for B. This takes any gating the
     * caller computes (per-detector PIE gates, µs-ALEX excitation phase, a folded clock) and
     * overrides ::set_stream_a / ::set_stream_b. Empty masks restore the streams.
     */
    void set_masks(unsigned char* mask_a, int n_mask_a, unsigned char* mask_b, int n_mask_b);

    /**
     * @brief Slice on this per-photon time axis instead of the macro times.
     *
     * One value per photon of the TTTR stream, for example folded µs-ALEX times or
     * alternation-period indices. The values need not be monotonic within a burst. An empty
     * axis restores the macro times.
     */
    void set_time_axis(long long* time_axis, int n_time_axis);

    /**
     * @brief The slice statistic: ::RATIO (default) or ::ANISOTROPY of A = parallel and
     *        B = perpendicular photons with the G factor and the l1, l2 polarisation mixing.
     *
     * Every statistic is a function of the slice fraction n_A / N, so ::static_bound simulates
     * the same binomial slices and transforms them. ::get_ratio stays the pooled fraction.
     */
    void set_statistic(int statistic, double g_factor = 1.0, double l1 = 0.0, double l2 = 0.0);

    /// The statistic of n_A out of n photons (the configured transform).
    double transform(double a, double n) const;

    /**
     * @brief Slice every burst and reduce its slices.
     * @param bursts Inclusive photon index ranges as an (n_bursts, 2) array.
     * @param slice_mode ::PHOTONS or ::TIME.
     * @param photons_per_slice Photons per slice (::PHOTONS).
     * @param window_ticks Window length in time-axis units (::TIME).
     * @param align_ticks Window grid alignment in time-axis units (::TIME, 0: none).
     * @param align_offset Added to the time axis before aligning (alternation offset).
     * @param min_photons Fewest photons a slice needs to count.
     * @param min_slices Fewest counted slices a burst needs to be tested.
     * @param ddof Delta degrees of freedom of the slice SD (1: sample SD, 0: population SD).
     */
    void compute(
        long long* bursts, int n_bursts, int n_cols,
        int slice_mode = PHOTONS,
        int photons_per_slice = 5,
        double window_ticks = 0.0,
        double align_ticks = 0.0,
        double align_offset = 0.0,
        int min_photons = 1,
        int min_slices = 2,
        int ddof = 1
    );

    /// Mean of the slice statistic per burst (NaN: untested); alias of ::get_result.
    const std::vector<double>& get_mean() const { return get_result(); }
    /// SD of the slice statistic per burst (NaN: untested).
    const std::vector<double>& get_sd() const { return sd_; }
    /// Pooled ratio sum n_A / sum N per burst (NaN: untested).
    const std::vector<double>& get_ratio() const { return ratio_; }
    /// Binomial chi-square per burst (NaN: untested, or pooled ratio 0 or 1).
    const std::vector<double>& get_chi2() const { return chi2_; }
    /// Counted slices per burst.
    const std::vector<int>& get_n_slices() const { return n_slices_; }
    /// Mean photon number of the counted slices per burst (NaN: untested).
    const std::vector<double>& get_mean_slice_photons() const { return mean_photons_; }
    /// Photon number of every counted slice of every tested burst, in burst order.
    const std::vector<int>& get_slice_photons() const { return slice_photons_; }
    /// Stream-A photon number of every counted slice (aligned with ::get_slice_photons).
    const std::vector<int>& get_slice_a() const { return slice_a_; }
    /// The statistic of the pooled counts per burst (the ratio itself for ::RATIO; NaN: untested).
    const std::vector<double>& get_pooled_statistic() const { return pooled_; }
    /// Duration of every counted slice in time-axis units: last minus first photon of a photon
    /// slice, the window length of a time window (aligned with ::get_slice_photons).
    const std::vector<double>& get_slice_duration() const { return slice_duration_; }
    /// Burst index of every counted slice (aligned with ::get_slice_photons).
    const std::vector<int>& get_slice_owner() const { return slice_owner_; }
    /// 1 where a burst is tested (``min_slices`` counted slices), else 0.
    std::vector<int> get_tested() const;

    /**
     * @brief Per-burst static bound: quantile of the slice SD of a simulated static molecule.
     * @param quantile Quantile of the simulated SDs (linear interpolation, as numpy).
     * @param n_mc Simulations per burst.
     * @param seed Seed of the counter-based RNG.
     * @param dispersion Shared dispersion phi; the bound is scaled by sqrt(phi).
     * @param ddof Delta degrees of freedom of the simulated SD (use the one of ::compute).
     * @param background_a, background_b Background rates of A and B in photons per time-axis
     *        unit. Each slice is then a mixture: a fraction f = (b_A + b_B) d / N of its photons
     *        is background (A with probability b_A / (b_A + b_B)), the rest signal at the ratio
     *        that keeps the burst's pooled ratio. A photon slice spans N - 1 intervals, so its
     *        duration d is scaled by N / (N - 1). 0, 0: the plain binomial.
     * @return One bound per burst (NaN where untested). Requires ::compute.
     */
    std::vector<double> static_bound(
        double quantile = 0.95, int n_mc = 300, unsigned int seed = 11,
        double dispersion = 1.0, int ddof = 1,
        double background_a = 0.0, double background_b = 0.0
    ) const;

    /// Shot-noise SD of a static ratio: sqrt(phi * p (1 - p) / n), elementwise.
    static std::vector<double> shot_noise_sd(
        const std::vector<double>& ratio, const std::vector<double>& photons,
        double dispersion = 1.0
    );

    /// 1 where sd <= bound, 0 where sd > bound, -1 where either is not finite.
    static std::vector<int> classify(const std::vector<double>& sd, const std::vector<double>& bound);

private:
    static constexpr const char* STREAM_A = "a";
    static constexpr const char* STREAM_B = "b";

    int statistic_ = RATIO;
    double g_factor_ = 1.0;
    double l1_ = 0.0;
    double l2_ = 0.0;
    int slice_mode_ = PHOTONS;
    std::vector<double> pooled_;
    std::vector<double> slice_duration_;
    std::vector<int64_t> time_axis_;
    std::vector<uint8_t> mask_a_;
    std::vector<uint8_t> mask_b_;
    std::vector<double> sd_;
    std::vector<double> ratio_;
    std::vector<double> chi2_;
    std::vector<int> n_slices_;
    std::vector<double> mean_photons_;
    std::vector<int> slice_photons_;
    std::vector<int> slice_a_;
    std::vector<int> slice_owner_;
};

} // namespace tttrlib

#endif // TTTRLIB_BURSTVARIANCE_H
