// SPDX-License-Identifier: BSD-3-Clause
//
// The fused burst pipeline. Reference implementations it reproduces, stage by
// stage (all in ChiSurf, chisurf/...):
//   search         core/fluorescence/burst/tttrlib_search.py (_search_detectors) and
//                  TTTR::burst_search_cusum_sprt / burst_search_sliding_window
//   selection      plugins/burst/burst_selection/api/selection.py (apply_photon_filters,
//                  find_bursts, drop_short_bursts) and core/math/signal.py
//   features       core/fio/fluorescence/burst.py (_static_columns, _span_columns)
//   decays         plugins/burst/burst_mle_analysis/_mp_worker.py (burst_counts)
//   strict bg      core/fluorescence/burst/irf_bg.py (strict_background_mask)
//   fits           _mp_worker.py (decay_rows, _fit_chunk, reject_reason, tail_lifetimes)
//                  and core/fluorescence/mle/fit2x.py (Fit2xSettings.to_fitted)
// test/python/burstpipeline/ checks each against them.
#include "BurstPipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>

#include "DecayFitModel.h"
#include "DecayFitProblem.h"

using json = nlohmann::json;

namespace tttrlib {

namespace {

constexpr long long kEnd = std::numeric_limits<long long>::max();
const double kNaN = std::numeric_limits<double>::quiet_NaN();

double now_s() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/*! Worker count: explicit, else TTTRLIB_NUM_THREADS / OMP_NUM_THREADS, else the cores (max 8). */
int thread_count(int requested) {
    if (requested > 0) return requested;
    for (const char *name : {"TTTRLIB_NUM_THREADS", "OMP_NUM_THREADS"}) {
        const char *v = std::getenv(name);
        if (v != nullptr && std::atoi(v) > 0) return std::atoi(v);
    }
    unsigned hc = std::thread::hardware_concurrency();
    return std::max(1, std::min(8, static_cast<int>(hc == 0 ? 1 : hc)));
}

struct Gate { int lo, hi; };  // half-open [lo, hi) in raw micro-time channels

struct DetectorDef {
    std::string name;
    std::vector<int> chs;
    std::vector<Gate> gates;  // empty = every micro time
};

struct WindowDef { std::string name; int lo, hi; };

std::vector<int> int_list(const json &j) {
    std::vector<int> out;
    if (j.is_array()) for (const auto &v : j) out.push_back(v.get<int>());
    return out;
}

std::vector<Gate> gate_list(const json &j) {
    std::vector<Gate> out;
    if (!j.is_array()) return out;
    for (const auto &g : j) {
        if (g.is_array() && g.size() >= 2) out.push_back({g[0].get<int>(), g[1].get<int>()});
    }
    return out;
}

DetectorDef detector_from(const json &j) {
    DetectorDef d;
    d.name = j.value("name", std::string());
    d.chs = int_list(j.value("chs", json::array()));
    d.gates = gate_list(j.value("gates", json::array()));
    return d;
}

bool in_gates(const std::vector<Gate> &gates, int m) {
    if (gates.empty()) return true;
    for (const auto &g : gates) if (m >= g.lo && m < g.hi) return true;
    return false;
}

bool has_channel(const std::vector<int> &chs, int c) {
    return std::find(chs.begin(), chs.end(), c) != chs.end();
}

// ---------------------------------------------------------------------------------------
// The photon classifier: (routing channel, micro time) -> one byte.
// ---------------------------------------------------------------------------------------

struct Combo {
    uint32_t det = 0;     // bit d: photon belongs to detector d (channel and gate)
    uint32_t win = 0;     // bit w: micro time inside window w
    int slot = -1;        // channel slot, -1 = a channel no definition names
    bool subset = false;  // photon is searched
    bool pre = true;      // photon passes the selection pre-filter
};

/*!
 * Every channel any definition names gets a slot; the micro-time axis is cut at
 * every gate and window bound, so within a (slot, piece) every photon belongs to
 * the same detectors and windows. The table maps (slot, micro) to the code of that
 * combination; micro times past the last bound share the last column.
 */
struct Classifier {
    std::vector<int> slot_of;          // 256 entries
    std::vector<int> slot_channel;
    int n_slots = 0;
    int width = 1;                     // micro times >= width - 1 use column width - 1
    std::vector<uint8_t> lut;          // (n_slots + 1) * width
    std::vector<Combo> combos;

    void build(const std::vector<DetectorDef> &detectors, const std::vector<WindowDef> &windows,
               const std::vector<DetectorDef> &subset_defs, bool subset_all,
               const std::vector<int> &pre_channels, const std::vector<Gate> &pre_gates,
               const std::vector<int> &extra_channels) {
        slot_of.assign(256, -1);
        auto add = [&](int c) {
            const int k = static_cast<unsigned char>(static_cast<signed char>(c));
            if (slot_of[k] < 0) { slot_of[k] = n_slots++; slot_channel.push_back(c); }
        };
        for (const auto &d : detectors) for (int c : d.chs) add(c);
        for (const auto &d : subset_defs) for (int c : d.chs) add(c);
        for (int c : pre_channels) add(c);
        for (int c : extra_channels) add(c);
        int bound = 1;
        auto widen = [&](int v) { bound = std::max(bound, v + 1); };
        for (const auto &d : detectors) for (const auto &g : d.gates) { widen(g.lo); widen(g.hi); }
        for (const auto &d : subset_defs) for (const auto &g : d.gates) { widen(g.lo); widen(g.hi); }
        for (const auto &w : windows) { widen(w.lo); widen(w.hi); }
        for (const auto &g : pre_gates) { widen(g.lo); widen(g.hi); }
        width = std::min(bound + 1, 65537);
        lut.assign(static_cast<size_t>(n_slots + 1) * width, 0);
        combos.clear();
        for (int s = 0; s <= n_slots; ++s) {
            const int ch = s < n_slots ? slot_channel[s] : -100000;
            for (int m = 0; m < width; ++m) {
                Combo c;
                c.slot = s < n_slots ? s : -1;
                for (size_t d = 0; d < detectors.size(); ++d) {
                    if (has_channel(detectors[d].chs, ch) && in_gates(detectors[d].gates, m))
                        c.det |= (1u << d);
                }
                for (size_t w = 0; w < windows.size(); ++w) {
                    if (m >= windows[w].lo && m < windows[w].hi) c.win |= (1u << w);
                }
                if (subset_all) {
                    c.subset = true;
                } else {
                    for (const auto &d : subset_defs) {
                        if (has_channel(d.chs, ch) && in_gates(d.gates, m)) c.subset = true;
                    }
                }
                c.pre = (pre_channels.empty() || has_channel(pre_channels, ch)) &&
                        (pre_gates.empty() || in_gates(pre_gates, m));
                int code = -1;
                for (size_t k = 0; k < combos.size(); ++k) {
                    const Combo &o = combos[k];
                    if (o.det == c.det && o.win == c.win && o.slot == c.slot &&
                        o.subset == c.subset && o.pre == c.pre) { code = static_cast<int>(k); break; }
                }
                if (code < 0) {
                    if (combos.size() >= 255)
                        throw std::invalid_argument("BurstPipeline: more than 255 photon classes");
                    combos.push_back(c);
                    code = static_cast<int>(combos.size()) - 1;
                }
                lut[static_cast<size_t>(s) * width + m] = static_cast<uint8_t>(code);
            }
        }
    }

    inline uint8_t code(signed char channel, unsigned short micro) const {
        const int s0 = slot_of[static_cast<unsigned char>(channel)];
        const int s = s0 < 0 ? n_slots : s0;
        const int m = micro < width ? micro : width - 1;
        return lut[static_cast<size_t>(s) * width + m];
    }
};

// ---------------------------------------------------------------------------------------
// Fits
// ---------------------------------------------------------------------------------------

struct FitEntry {
    std::string detector;
    int group = -1;
    std::string model;       // registry name, or "tail"
    bool tail = false;
    std::shared_ptr<const DecayFitModel> fit_model;
    DecayFitProblem prototype;
    DecayFitConstraints constraints;
    std::vector<double> x0;
    int n_par = 0, n_res = 0;
    int n = 0;               // bins per polarisation of the histograms
    int nf = 0;              // bins per polarisation the model sees (window stop)
    int fit_start = 0;       // bins before this are not scored
    int sb = 0, eb = 0, vh_sb = 0, vh_eb = 0;
    int shift = 0;
    long long min_photons = 0;
    bool mirror = false;
    double dt = 1.0;
    int tail_start = -1;     // -1: one bin after the peak of the summed decay in [sb, eb)
    int converged_index = -1;
    int reject_lo = 0, reject_hi = 0;
    std::vector<double> burst_backgrounds;   // per-burst background, n_rows * (2 * nf)
    int burst_background_rows = 0;
};

struct FitOutputs {
    std::vector<long long> ng_p, ng_s, reason;
    std::vector<double> objective;
    std::vector<double> x;   // n_rows * n_par
    std::vector<double> r;   // n_rows * n_res
    void resize(size_t rows, int n_par, int n_res) {
        ng_p.assign(rows, 0); ng_s.assign(rows, 0); reason.assign(rows, 1);
        objective.assign(rows, kNaN);
        x.assign(rows * static_cast<size_t>(std::max(n_par, 0)), kNaN);
        r.assign(rows * static_cast<size_t>(std::max(n_res, 0)), kNaN);
    }
};

/*! `_mp_worker.reject_reason`. */
int reject_reason(const FitEntry &e, const double *x, int nx, double converged) {
    const int tau_index = e.tail ? 1 : 0;
    if (nx <= tau_index || !std::isfinite(x[tau_index])) return 3;
    if (e.converged_index >= 0 && std::isfinite(converged) && converged <= 0) return 2;
    const double tau = x[tau_index];
    if (tau <= 0.5 * e.dt || tau <= 0.0) return 4;
    const double span = static_cast<double>(e.reject_hi - e.reject_lo) * e.dt;
    if (span > 0 && tau > span) return 5;
    if (e.model == "fit23" && nx > 1 && std::isfinite(x[1]) && x[1] >= 0.99) return 6;
    return 0;
}

/*! `_mp_worker.tail_lifetimes` for one row. */
double tail_lifetime(const double *tail, int m, double dt) {
    if (m < 2 || dt <= 0) return kNaN;
    double n = 0.0, s = 0.0;
    for (int k = 0; k < m; ++k) { n += tail[k]; s += tail[k] * ((k + 0.5) * dt); }
    const double T = m * dt;
    const double mean = s / (n > 0 ? n : 1.0);
    if (!(n >= 2) || mean >= 0.5 * T * 0.999) return kNaN;
    double lo = 1e-4, hi = 1e4;
    for (int it = 0; it < 80; ++it) {
        const double mid = std::sqrt(lo * hi);
        const double r = T / mid;
        const double value = r < 700 ? mid - T / std::expm1(std::min(r, 700.0)) : mid;
        if (value < mean) lo = mid; else hi = mid;
    }
    return std::sqrt(lo * hi);
}

/*! CSR rows of one decay group: rows [row0, row0 + n_rows) of the batch. */
struct CsrView {
    const long long *indptr = nullptr;   // n_rows + 1 entries, absolute offsets
    const int *bins = nullptr;
    const int *counts = nullptr;
};

/*! Worker scratch: one per thread. */
struct FitScratch {
    std::vector<long long> cp, cs;
    std::vector<double> row, x, res, tail;
    DecayFitProblem problem;
    bool have_problem = false;
};

/*!
 * Fit burst \p b of \p e from its CSR row. Mirrors `_fit_chunk` + `decay_rows` +
 * `Fit2xSettings.to_fitted(score_only=True)`: VV in [sb, eb), VH in [vh_sb, vh_eb)
 * rolled by the shift, every half cut at the window's stop and zero before its start.
 */
void fit_one(const FitEntry &e, const CsrView &v, long long local_row, long long burst,
             FitScratch &w, FitOutputs &o) {
    const long long out_row = local_row;
    const int n = e.n;
    w.cp.assign(n, 0);
    w.cs.assign(n, 0);
    for (long long k = v.indptr[local_row]; k < v.indptr[local_row + 1]; ++k) {
        const int c = v.bins[k];
        if (c < n) w.cp[c] += v.counts[k]; else if (c - n < n) w.cs[c - n] += v.counts[k];
    }
    if (e.mirror) w.cs = w.cp;
    const int s0 = std::max(0, e.sb), s1 = std::min(n, e.eb);
    const int t0 = std::max(0, e.vh_sb), t1 = std::min(n, e.vh_eb);
    long long cp_sum = 0, cs_sum = 0;
    for (int j = s0; j < s1; ++j) cp_sum += w.cp[j];
    for (int j = t0; j < t1; ++j) cs_sum += w.cs[j];
    if (e.mirror) cs_sum = 0;
    o.ng_p[out_row] = cp_sum;
    o.ng_s[out_row] = cs_sum;
    o.reason[out_row] = 1;
    if (cp_sum + cs_sum < e.min_photons) return;

    double *x = o.x.data() + out_row * e.n_par;
    if (e.tail) {
        const int start = std::min(std::max(e.tail_start, s0), s1);
        const int m = std::max(0, s1 - start);
        w.tail.assign(m, 0.0);
        for (int k = 0; k < m; ++k) {
            w.tail[k] = static_cast<double>(w.cp[start + k]);
            if (!e.mirror) w.tail[k] += static_cast<double>(w.cs[start + k]);
        }
        x[0] = start;
        x[1] = tail_lifetime(w.tail.data(), m, e.dt);
        x[2] = kNaN;
        o.reason[out_row] = reject_reason(e, x, 3, kNaN);
        return;
    }

    if (!w.have_problem) { w.problem = e.prototype; w.have_problem = true; }
    DecayFitProblem &p = w.problem;
    const int nf = e.nf;
    std::fill(p.data.begin(), p.data.end(), 0.0);
    const int lo = std::max(e.fit_start, 0);
    for (int j = std::max(s0, lo); j < std::min(s1, nf); ++j) p.data[j] = static_cast<double>(w.cp[j]);
    for (int j = std::max(t0, lo); j < std::min(t1, nf); ++j) {
        int src = j;
        if (e.shift) { src = (j - e.shift) % n; if (src < 0) src += n; }
        p.data[nf + j] = static_cast<double>(w.cs[src]);
    }
    if (e.burst_background_rows > 0 && burst < e.burst_background_rows) {
        const size_t len = static_cast<size_t>(2 * nf);
        p.background.assign(e.burst_backgrounds.begin() + burst * len,
                            e.burst_backgrounds.begin() + (burst + 1) * len);
    }
    p.reset_model();
    w.x.assign(e.x0.begin(), e.x0.end());
    w.res.assign(static_cast<size_t>(std::max(e.n_res, 0)), kNaN);
    double obj = kNaN;
    try {
        obj = e.fit_model->fit(w.x.data(), e.constraints, p, e.n_res > 0 ? w.res.data() : nullptr);
    } catch (const std::exception &) {
        o.reason[out_row] = 3;
        return;
    }
    for (int i = 0; i < e.n_par; ++i) x[i] = w.x[i];
    for (int i = 0; i < e.n_res; ++i) o.r[out_row * e.n_res + i] = w.res[i];
    o.objective[out_row] = obj;
    const double conv = e.converged_index >= 0 && e.converged_index < e.n_res
                            ? w.res[e.converged_index] : kNaN;
    o.reason[out_row] = reject_reason(e, x, e.n_par, conv);
}

FitEntry fit_entry_from(const json &j, const BurstPipelineResult &res) {
    FitEntry e;
    e.detector = j.at("detector").get<std::string>();
    e.group = j.contains("group") ? j["group"].get<int>() : res.decay_group_of(e.detector);
    if (e.group < 0 || e.group >= static_cast<int>(res.groups_.size()))
        throw std::invalid_argument("BurstPipeline.fit: no decay group for detector " + e.detector);
    e.model = j.value("model", std::string("fit23"));
    e.tail = e.model == "tail";
    e.n = j.value("n", res.groups_[e.group].n_bins);
    if (e.n != res.groups_[e.group].n_bins)
        throw std::invalid_argument("BurstPipeline.fit: n of " + e.detector +
                                    " differs from the decays' bins per polarisation");
    e.fit_start = j.value("fit_start", 0);
    e.nf = j.value("fit_stop", -1);
    if (e.nf <= 0 || e.nf > e.n) e.nf = e.n;
    e.sb = j.value("sb", 0); e.eb = j.value("eb", e.n);
    e.vh_sb = j.value("vh_sb", e.sb); e.vh_eb = j.value("vh_eb", e.eb);
    e.shift = j.value("shift", 0);
    e.min_photons = j.value("min_photons", 0LL);
    e.mirror = j.value("mirror", res.groups_[e.group].mirror);
    e.dt = j.value("dt", 1.0);
    e.tail_start = j.value("tail_start", -1);
    e.converged_index = j.value("converged_index", -1);
    e.reject_lo = j.value("reject_lo", e.sb);
    e.reject_hi = j.value("reject_hi", e.eb);
    if (e.tail) {
        e.n_par = 3; e.n_res = 0;
        if (e.tail_start < 0) {
            const auto &sum = res.groups_[e.group].sum;
            const int s0 = std::max(0, e.sb), s1 = std::min(e.n, e.eb);
            long long best = -1; int at = s0 - 1;
            for (int k = s0; k < s1; ++k) {
                const long long v = sum[k] + sum[e.n + k];
                if (v > best) { best = v; at = k; }
            }
            e.tail_start = best > 0 ? at + 1 : e.sb;
        }
        return e;
    }
    std::vector<double> setup = j.value("setup", std::vector<double>());
    std::vector<double> irf = j.value("irf", std::vector<double>());
    std::vector<double> bg = j.value("background", std::vector<double>());
    if (irf.size() != static_cast<size_t>(2 * e.nf))
        throw std::invalid_argument("BurstPipeline.fit: irf of " + e.detector +
                                    " must hold 2 * fitted bins values");
    if (bg.empty()) bg.assign(irf.size(), 0.0);
    e.fit_model = make_decay_fit(e.model, setup, irf);
    e.prototype = DecayFitProblem(2, e.nf, e.dt);
    e.prototype.irf = irf;
    e.prototype.background = bg;
    e.prototype.require_valid();
    e.n_par = e.fit_model->n_parameters(e.prototype);
    e.n_res = e.fit_model->n_results(e.prototype);
    e.x0 = j.value("x0", std::vector<double>());
    if (static_cast<int>(e.x0.size()) != e.n_par)
        throw std::invalid_argument("BurstPipeline.fit: x0 of " + e.detector +
                                    " must hold every parameter of " + e.model);
    std::vector<int> link = j.value("link", std::vector<int>(e.n_par, 0));
    e.constraints = DecayFitConstraints(link);
    return e;
}

/*! A batch of bursts to fit: rows [row0, row0 + n) of every entry's group. */
struct FitBatch {
    long long row0 = 0, n = 0;
    std::vector<CsrView> views;                    // per entry
    std::vector<std::vector<long long>> own_indptr; // a snapshot, when the result still grows
    std::vector<std::vector<int>> own_bins, own_counts;
    std::vector<FitOutputs> out;                    // per entry, n rows
};

/*! A fixed pool of fit workers fed batch by batch. */
class FitPool {
public:
    FitPool(const std::vector<FitEntry> &entries, int threads,
            std::atomic<long long> &done, std::atomic<bool> &cancel)
        : entries_(entries), done_(done), cancel_(cancel) {
        for (int t = 0; t < threads; ++t) workers_.emplace_back([this] { work(); });
    }
    ~FitPool() { finish(); }

    void submit(std::shared_ptr<FitBatch> b) {
        b->out.resize(entries_.size());
        for (size_t e = 0; e < entries_.size(); ++e)
            b->out[e].resize(static_cast<size_t>(b->n), entries_[e].n_par, entries_[e].n_res);
        {
            std::lock_guard<std::mutex> g(m_);
            all_.push_back(b);
            queue_.push_back(std::move(b));
        }
        cv_.notify_one();
    }

    void finish() {
        {
            std::lock_guard<std::mutex> g(m_);
            if (closed_) return;
            closed_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_) if (t.joinable()) t.join();
    }

    std::string error() const { return error_; }

    /*! Every batch's outputs, in burst order, as one set of columns per entry. */
    std::vector<FitOutputs> gather(long long rows) {
        std::vector<FitOutputs> out(entries_.size());
        for (size_t e = 0; e < entries_.size(); ++e)
            out[e].resize(static_cast<size_t>(rows), entries_[e].n_par, entries_[e].n_res);
        for (const auto &b : all_) {
            for (size_t e = 0; e < entries_.size(); ++e) {
                const FitOutputs &o = b->out[e];
                const int np = entries_[e].n_par, nr = entries_[e].n_res;
                for (long long i = 0; i < b->n; ++i) {
                    const long long r = b->row0 + i;
                    if (r >= rows) break;
                    out[e].ng_p[r] = o.ng_p[i]; out[e].ng_s[r] = o.ng_s[i];
                    out[e].reason[r] = o.reason[i]; out[e].objective[r] = o.objective[i];
                    for (int k = 0; k < np; ++k) out[e].x[r * np + k] = o.x[i * np + k];
                    for (int k = 0; k < nr; ++k) out[e].r[r * nr + k] = o.r[i * nr + k];
                }
            }
        }
        return out;
    }

private:
    void work() {
        std::vector<FitScratch> scratch(entries_.size());
        for (;;) {
            std::shared_ptr<FitBatch> b;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return closed_ || !queue_.empty(); });
                if (queue_.empty()) return;
                b = queue_.front();
                queue_.pop_front();
            }
            if (cancel_.load()) continue;
            try {
                for (long long i = 0; i < b->n; ++i) {
                    for (size_t e = 0; e < entries_.size(); ++e)
                        fit_one(entries_[e], b->views[e], i, b->row0 + i, scratch[e], b->out[e]);
                    done_.fetch_add(1);
                }
            } catch (const std::exception &ex) {
                std::lock_guard<std::mutex> g(m_);
                error_ = ex.what();
            }
        }
    }

    const std::vector<FitEntry> &entries_;
    std::vector<std::shared_ptr<FitBatch>> all_;
    std::atomic<long long> &done_;
    std::atomic<bool> &cancel_;
    std::vector<std::thread> workers_;
    std::deque<std::shared_ptr<FitBatch>> queue_;
    std::mutex m_;
    std::condition_variable cv_;
    bool closed_ = false;
    std::string error_;
};

void store_fit_columns(BurstPipelineResult &res, const std::vector<FitEntry> &entries,
                       const std::vector<FitOutputs> &outputs) {
    for (size_t k = 0; k < entries.size(); ++k) {
        const FitEntry &e = entries[k];
        const FitOutputs &o = outputs[k];
        const std::string p = "fit|" + e.detector + "|";
        res.i64_[p + "ng_p"] = o.ng_p;
        res.i64_[p + "ng_s"] = o.ng_s;
        res.i64_[p + "reason"] = o.reason;
        res.f64_[p + "objective"] = o.objective;
        const size_t rows = o.ng_p.size();
        for (int i = 0; i < e.n_par; ++i) {
            std::vector<double> c(rows);
            for (size_t r = 0; r < rows; ++r) c[r] = o.x[r * e.n_par + i];
            res.f64_[p + "x|" + std::to_string(i)] = std::move(c);
        }
        for (int i = 0; i < e.n_res; ++i) {
            std::vector<double> c(rows);
            for (size_t r = 0; r < rows; ++r) c[r] = o.r[r * e.n_res + i];
            res.f64_[p + "r|" + std::to_string(i)] = std::move(c);
        }
    }
}

}  // namespace

// =======================================================================================
// BurstPipelineResult
// =======================================================================================

std::vector<std::string> BurstPipelineResult::column_names() const {
    std::vector<std::string> out;
    for (const auto &kv : f64_) out.push_back(kv.first);
    for (const auto &kv : i64_) out.push_back(kv.first);
    return out;
}

bool BurstPipelineResult::has_column(const std::string &name) const { return f64_.count(name) > 0; }

bool BurstPipelineResult::has_index_column(const std::string &name) const { return i64_.count(name) > 0; }

void BurstPipelineResult::column(const std::string &name, double **view, int *n) {
    auto it = f64_.find(name);
    if (it == f64_.end()) throw std::out_of_range("BurstPipelineResult: no float64 column " + name);
    *view = it->second.data();
    *n = static_cast<int>(it->second.size());
}

void BurstPipelineResult::index_column(const std::string &name, long long **view, int *n) {
    auto it = i64_.find(name);
    if (it == i64_.end()) throw std::out_of_range("BurstPipelineResult: no int64 column " + name);
    *view = it->second.data();
    *n = static_cast<int>(it->second.size());
}

std::string BurstPipelineResult::decay_group_name(int g) const { return groups_.at(g).name; }

std::vector<int> BurstPipelineResult::decay_group_channels(int g) const { return groups_.at(g).channels; }

int BurstPipelineResult::decay_group_of(const std::string &detector) const {
    for (size_t d = 0; d < detector_names_.size(); ++d)
        if (detector_names_[d] == detector) return detector_group_[d];
    return -1;
}

int BurstPipelineResult::decay_n_bins(int g) const { return groups_.at(g).n_bins; }

void BurstPipelineResult::decay_indptr(int g, long long **view, int *n) {
    auto &v = groups_.at(g).indptr; *view = v.data(); *n = static_cast<int>(v.size());
}

void BurstPipelineResult::decay_bins(int g, int **view, int *n) {
    auto &v = groups_.at(g).bins; *view = v.data(); *n = static_cast<int>(v.size());
}

void BurstPipelineResult::decay_counts(int g, int **view, int *n) {
    auto &v = groups_.at(g).counts; *view = v.data(); *n = static_cast<int>(v.size());
}

void BurstPipelineResult::decay_sum(int g, long long **view, int *n) {
    auto &v = groups_.at(g).sum; *view = v.data(); *n = static_cast<int>(v.size());
}

void BurstPipelineResult::outside_histogram(long long **view, int *n) {
    *view = outside_hist_.data(); *n = static_cast<int>(outside_hist_.size());
}

long long BurstPipelineResult::nbytes() const {
    long long b = 0;
    for (const auto &kv : f64_) b += static_cast<long long>(kv.second.size() * 8);
    for (const auto &kv : i64_) b += static_cast<long long>(kv.second.size() * 8);
    for (const auto &g : groups_)
        b += static_cast<long long>(g.indptr.size() * 8 + g.bins.size() * 4 + g.counts.size() * 4 +
                                    g.sum.size() * 8);
    b += static_cast<long long>(outside_hist_.size() * 8);
    return b;
}

// =======================================================================================
// BurstPipeline::Impl -- the configuration, parsed once
// =======================================================================================

struct BurstPipeline::Impl {
    std::vector<DetectorDef> detectors;
    std::vector<WindowDef> windows;
    // search
    std::string algorithm = "cusum_sprt";
    bool search_all = false;
    std::vector<DetectorDef> search_defs;
    int cusum_min_photons = 20;
    double background_cps = 0.0, sbr = 4.0, alpha = 0.05, beta = 0.05;
    int sw_L = 20, sw_m = 10;
    double sw_T = 5e-4;
    // selection
    bool filter_active = true;
    bool invert = false;
    int max_gap = 0;
    long long min_photons = 2;
    bool dt_min_active = false, dt_max_active = false;
    double dt_min_ms = 0.0, dt_max_ms = 0.0;
    std::vector<int> pre_channels;
    std::vector<Gate> pre_gates;
    // decays
    int binning = 1;
    int n_bins = 0;
    bool features = true;
    bool confidence = true;
    double micro_ns = -1.0;     // < 0: from the header
    double macro_res = 0.0;     // 0: from the header
    double feature_res = 0.0;   // 0: macro_res; the resolution durations are written in
    long long chunk = 1 << 20;
    int threads = 0;
    json fits;                  // fused fits (optional)
    Classifier cls;
    std::vector<std::vector<int>> group_chs;
    std::vector<int> det_group;
    std::vector<std::vector<int8_t>> group_role;  // [group][slot] -1 / 0 P / 1 S
    std::vector<int> hist_channels;
    std::map<int, std::vector<double>> burst_backgrounds;  // fit index -> rows
    std::map<int, int> burst_background_rows;

    void parse(const json &j) {
        for (const auto &d : j.value("detectors", json::array())) detectors.push_back(detector_from(d));
        if (detectors.size() > 32) throw std::invalid_argument("BurstPipeline: at most 32 detectors");
        for (const auto &w : j.value("windows", json::array()))
            windows.push_back({w.value("name", std::string()), w.at("lo").get<int>(), w.at("hi").get<int>()});
        if (windows.size() > 32) throw std::invalid_argument("BurstPipeline: at most 32 windows");
        const json s = j.value("search", json::object());
        algorithm = s.value("algorithm", std::string("cusum_sprt"));
        if (algorithm != "cusum_sprt" && algorithm != "sliding_window")
            throw std::invalid_argument("BurstPipeline: search algorithm '" + algorithm +
                                        "' is not fused; use cusum_sprt or sliding_window");
        const std::string scope = s.value("scope", std::string("all"));
        search_all = scope == "all";
        if (!search_all) {
            if (s.contains("detectors")) {
                for (const auto &d : s["detectors"]) search_defs.push_back(detector_from(d));
            } else {
                search_defs = detectors;
            }
        }
        const json p = s.value("parameters", json::object());
        cusum_min_photons = p.value("min_photons", 20);
        background_cps = p.value("background_cps", 0.0);
        sbr = p.value("signal_to_background_ratio", 4.0);
        alpha = p.value("alpha", 0.05);
        beta = p.value("beta", 0.05);
        sw_L = p.value("L", 20);
        sw_m = p.value("m", 10);
        sw_T = p.value("T", 5e-4);
        const json sel = j.value("selection", json::object());
        filter_active = sel.value("filter_active", true);
        invert = sel.value("invert", false);
        max_gap = sel.value("max_gap", 0);
        min_photons = std::max<long long>(2, sel.value("min_photons", 2LL));
        dt_min_active = sel.value("dt_min_active", false);
        dt_max_active = sel.value("dt_max_active", false);
        dt_min_ms = sel.value("dt_min_ms", 0.0);
        dt_max_ms = sel.value("dt_max_ms", 0.0);
        pre_channels = int_list(sel.value("channels", json::array()));
        pre_gates = gate_list(sel.value("micro_time_ranges", json::array()));
        const json dc = j.value("decays", json::object());
        binning = std::max(1, dc.value("binning", 1));
        n_bins = dc.value("n_bins", 0);
        features = j.value("features", true);
        confidence = j.value("confidence", true);
        micro_ns = j.value("micro_time_resolution_ns", -1.0);
        macro_res = j.value("macro_time_resolution", 0.0);
        feature_res = j.value("feature_macro_time_resolution", 0.0);
        chunk = std::max<long long>(1024, j.value("chunk_size", 1LL << 20));
        threads = j.value("n_threads", 0);
        fits = j.value("fits", json::array());
        hist_channels.clear();
        for (const auto &d : detectors) for (int c : d.chs)
            if (!has_channel(hist_channels, c)) hist_channels.push_back(c);
        std::sort(hist_channels.begin(), hist_channels.end());
        cls.build(detectors, windows, search_defs, search_all, pre_channels, pre_gates, hist_channels);
        // decay groups: one per distinct channel list
        for (const auto &d : detectors) {
            int g = -1;
            for (size_t k = 0; k < group_chs.size(); ++k) if (group_chs[k] == d.chs) g = static_cast<int>(k);
            if (g < 0) { group_chs.push_back(d.chs); g = static_cast<int>(group_chs.size()) - 1; }
            det_group.push_back(g);
        }
        for (const auto &chs : group_chs) {
            std::vector<int8_t> role(cls.n_slots + 1, -1);
            const bool mirror = chs.size() < 2;
            for (size_t i = 0; i < chs.size(); ++i) {
                const int s = cls.slot_of[static_cast<unsigned char>(static_cast<signed char>(chs[i]))];
                if (s >= 0) role[s] = mirror ? 0 : static_cast<int8_t>(i % 2);
            }
            group_role.push_back(role);
        }
    }
};

// =======================================================================================
// One run over the records
// =======================================================================================

namespace {

struct Stream { uint32_t det; uint32_t win; };  // win == 0: the whole detector

/*!
 * The state of one `run`: the cursor over the records, the buffers of what is still
 * needed, the selection runs, and the result being filled.
 */
struct Run {
    const BurstPipeline::Impl &cfg;
    TTTR &t;
    BurstPipelineResult &res;
    std::atomic<long long> &photons_done;
    std::atomic<long long> &bursts_done;
    std::atomic<bool> &cancel;
    long long N = 0;
    double res_s = 1.0;      // macro-time resolution, seconds (search, delta-T)
    double res_f = 1.0;      // the resolution durations and times are written in
    double micro_ns = 0.0;

    // cursor and the per-photon codes still needed
    long long pos = 0;
    std::vector<uint8_t> codes;
    long long code_base = 0;
    long long peak_codes = 0;

    // detector subset, for the search
    std::vector<unsigned long long> sub_macro;
    std::vector<long long> sub_full;
    long long sub_base = 0;
    long long peak_sub = 0;

    // search bursts not yet consumed by the selection, full indices
    std::deque<std::pair<long long, long long>> search_bursts;
    long long n_search_bursts = 0;
    long long selected_photons = 0;
    long long next = 0;       // next photon the selection evaluates
    bool have_run = false;
    long long rs = 0, re = 0;

    // features
    std::vector<Stream> streams;
    std::vector<std::string> stream_names;
    struct StreamCols {
        std::vector<long long> first, last, n;
        std::vector<double> dur, mean_macro, rate, mean_micro;
    };
    std::vector<StreamCols> scols;
    std::vector<long long> s_first, s_last, s_n;
    std::vector<double> s_dur, s_mean_macro, s_rate;
    // histograms per channel (all photons, burst photons)
    std::vector<int> hist_slot;          // slot -> row, -1
    std::vector<std::vector<long long>> hist_all, hist_in;
    // decays
    std::vector<std::vector<long long>> scratch;   // per group, 2 * n_bins
    int n_bins = 0;
    // fused fits
    FitPool *pool = nullptr;
    std::vector<FitEntry> *entries = nullptr;
    long long submitted = 0;
    long long batch_rows = 256;

    Run(const BurstPipeline::Impl &c, TTTR &tt, BurstPipelineResult &r, std::atomic<long long> &pd,
        std::atomic<long long> &bd, std::atomic<bool> &cn)
        : cfg(c), t(tt), res(r), photons_done(pd), bursts_done(bd), cancel(cn) {}

    void init(int n_bins_) {
        N = static_cast<long long>(t.size());
        TTTRHeader *h = t.get_header();
        res_s = cfg.macro_res > 0 ? cfg.macro_res : (h ? h->get_macro_time_resolution() : 1.0);
        res_f = cfg.feature_res > 0 ? cfg.feature_res : res_s;
        micro_ns = cfg.micro_ns >= 0 ? cfg.micro_ns : (h ? h->get_micro_time_resolution() * 1e9 : 0.0);
        n_bins = n_bins_;
        // streams: detectors, then (window, detector)
        for (size_t d = 0; d < cfg.detectors.size(); ++d) {
            streams.push_back({1u << d, 0});
            stream_names.push_back(cfg.detectors[d].name);
        }
        for (size_t w = 0; w < cfg.windows.size(); ++w) {
            for (size_t d = 0; d < cfg.detectors.size(); ++d) {
                streams.push_back({1u << d, 1u << w});
                stream_names.push_back(cfg.windows[w].name + "@" + cfg.detectors[d].name);
            }
        }
        scols.resize(streams.size());
        hist_slot.assign(cfg.cls.n_slots + 1, -1);
        for (size_t k = 0; k < cfg.hist_channels.size(); ++k) {
            const int s = cfg.cls.slot_of[static_cast<unsigned char>(static_cast<signed char>(cfg.hist_channels[k]))];
            if (s >= 0) hist_slot[s] = static_cast<int>(k);
        }
        const int len0 = std::max(1, h ? static_cast<int>(h->get_number_of_micro_time_channels()) : 1);
        hist_all.assign(cfg.hist_channels.size(), std::vector<long long>(len0, 0));
        hist_in.assign(cfg.hist_channels.size(), std::vector<long long>(len0, 0));
        res.groups_.clear();
        for (size_t g = 0; g < cfg.group_chs.size(); ++g) {
            BurstPipelineResult::DecayGroup grp;
            for (size_t d = 0; d < cfg.detectors.size(); ++d)
                if (cfg.det_group[d] == static_cast<int>(g)) { grp.name = cfg.detectors[d].name; break; }
            grp.channels = cfg.group_chs[g];
            grp.mirror = grp.channels.size() < 2;
            grp.n_bins = n_bins;
            grp.sum.assign(2 * static_cast<size_t>(n_bins), 0);
            res.groups_.push_back(std::move(grp));
        }
        scratch.assign(cfg.group_chs.size(), std::vector<long long>(2 * static_cast<size_t>(n_bins), 0));
        res.detector_names_.clear();
        for (const auto &d : cfg.detectors) res.detector_names_.push_back(d.name);
        res.detector_group_ = cfg.det_group;
        res.micro_ns_ = micro_ns;
        res.macro_res_ = res_f;
        res.binning_ = cfg.binning;
    }

    inline void count_all(int slot, unsigned short micro) {
        const int row = hist_slot[slot < 0 ? cfg.cls.n_slots : slot];
        if (row < 0) return;
        auto &h = hist_all[row];
        if (micro >= h.size()) {
            for (auto &v : hist_all) v.resize(micro + 1, 0);
            for (auto &v : hist_in) v.resize(micro + 1, 0);
        }
        hist_all[row][micro] += 1;
    }

    /*! Classify the next chunk of records. False at the end of the stream. */
    bool advance() {
        if (pos >= N) return false;
        if (cancel.load()) { pos = N; return false; }
        const long long end = std::min(N, pos + cfg.chunk);
        for (long long i = pos; i < end; ++i) {
            const signed char ch = t.get_routing_channel_at(static_cast<size_t>(i));
            const unsigned short mi = t.get_micro_time_at(static_cast<size_t>(i));
            const uint8_t c = cfg.cls.code(ch, mi);
            codes.push_back(c);
            const Combo &k = cfg.cls.combos[c];
            count_all(k.slot, mi);
            if (k.subset) {
                sub_macro.push_back(t.get_macro_time_at(static_cast<size_t>(i)));
                sub_full.push_back(i);
            }
        }
        pos = end;
        photons_done.store(pos);
        peak_codes = std::max(peak_codes, static_cast<long long>(codes.size()));
        peak_sub = std::max(peak_sub, static_cast<long long>(sub_macro.size()));
        return true;
    }

    // ---- the detector subset, as the search sees it -----------------------------------
    bool sub_has(long long j) {
        while (j >= sub_base + static_cast<long long>(sub_macro.size())) {
            if (!advance()) return false;
        }
        return j >= sub_base;
    }
    inline unsigned long long sub_t(long long j) const { return sub_macro[static_cast<size_t>(j - sub_base)]; }
    inline long long sub_index(long long j) const { return sub_full[static_cast<size_t>(j - sub_base)]; }
    void sub_keep_from(long long j) {
        j = std::max<long long>(j, 0);
        if (j - sub_base > cfg.chunk) {
            const size_t k = static_cast<size_t>(j - sub_base);
            sub_macro.erase(sub_macro.begin(), sub_macro.begin() + static_cast<std::ptrdiff_t>(k));
            sub_full.erase(sub_full.begin(), sub_full.begin() + static_cast<std::ptrdiff_t>(k));
            sub_base = j;
        }
    }

    // ---- the selection ------------------------------------------------------------------
    inline uint8_t code_at(long long i) const { return codes[static_cast<size_t>(i - code_base)]; }

    void ensure_codes(long long i) {
        while (i >= pos && advance()) {}
    }

    inline bool dt_ok(long long i) {
        if (!cfg.dt_min_active && !cfg.dt_max_active) return true;
        double d = 0.0;
        if (i > 0) {
            const unsigned long long a = t.get_macro_time_at(static_cast<size_t>(i));
            const unsigned long long b = t.get_macro_time_at(static_cast<size_t>(i - 1));
            d = static_cast<double>(a - b) * res_s * 1000.0;
        }
        if (cfg.dt_min_active && !(d >= cfg.dt_min_ms)) return false;
        if (cfg.dt_max_active && !(d <= cfg.dt_max_ms)) return false;
        return true;
    }

    void selected(long long i) {
        const long long g = cfg.max_gap;
        if (have_run && (i == re + 1 || (g > 0 && i - re - 1 <= g + 1))) { re = i; return; }
        if (have_run) finalize();
        have_run = true; rs = re = i;
    }

    /*! Evaluate photons [next, F); every search burst starting before F is queued. */
    void select_to(long long F) {
        F = std::min(F, N);
        const bool sparse = cfg.filter_active && !cfg.invert;
        if (sparse) {
            while (!search_bursts.empty() && search_bursts.front().first < F) {
                auto sb = search_bursts.front();
                const long long a = std::max(sb.first, next);
                const long long b = std::min(sb.second, N - 1);
                if (b < a) { search_bursts.pop_front(); continue; }
                const long long b_now = std::min(b, F - 1);
                ensure_codes(b_now);
                for (long long i = a; i <= b_now; ++i) {
                    if (cfg.cls.combos[code_at(i)].pre && dt_ok(i)) selected(i);
                }
                next = b_now + 1;
                if (b_now == b) search_bursts.pop_front(); else break;
            }
            next = std::max(next, F);
        } else {
            ensure_codes(F - 1);
            for (long long i = next; i < F; ++i) {
                bool in = true;
                if (cfg.filter_active) {
                    while (!search_bursts.empty() && search_bursts.front().second < i) search_bursts.pop_front();
                    in = !search_bursts.empty() && search_bursts.front().first <= i;
                }
                bool s = in && dt_ok(i) && cfg.cls.combos[code_at(i)].pre;
                if (cfg.invert) s = !s;
                if (s) selected(i);
            }
            next = std::max(next, F);
        }
        if (have_run && next > re + std::max<long long>(cfg.max_gap, 0) + 2) finalize();
        trim_codes();
    }

    void trim_codes() {
        const long long keep = have_run ? std::min(rs, next) : next;
        if (keep - code_base > cfg.chunk) {
            const size_t k = static_cast<size_t>(keep - code_base);
            codes.erase(codes.begin(), codes.begin() + static_cast<std::ptrdiff_t>(k));
            code_base = keep;
        }
    }

    void finalize() {
        have_run = false;
        selected_photons += re - rs + 1;   // the mask after gap filling, short runs included
        if (re - rs + 1 < cfg.min_photons) return;
        emit(rs, re);
    }

    /*! The features and decays of burst [s, e]. */
    void emit(long long s, long long e) {
        ensure_codes(e);
        const unsigned long long t0 = t.get_macro_time_at(static_cast<size_t>(s));
        const unsigned long long t1 = t.get_macro_time_at(static_cast<size_t>(e));
        const double dur = static_cast<double>(t1 - t0) * res_f * 1e3;
        const long long n = e - s + 1;
        s_first.push_back(s);
        s_last.push_back(e);
        s_n.push_back(n);
        s_dur.push_back(dur);
        s_mean_macro.push_back((static_cast<double>(t1 + t0) / 2.0) * res_f * 1e3);
        s_rate.push_back(dur > 0 ? static_cast<double>(n) / dur : kNaN);

        const size_t ns = streams.size();
        std::vector<long long> f(ns, -1), l(ns, -1), cnt(ns, 0), msum(ns, 0);
        const int b = cfg.binning;
        for (long long i = s; i <= e; ++i) {
            const Combo &k = cfg.cls.combos[code_at(i)];
            const unsigned short mi = t.get_micro_time_at(static_cast<size_t>(i));
            if (cfg.features && k.det) {
                for (size_t q = 0; q < ns; ++q) {
                    const Stream &st = streams[q];
                    if ((k.det & st.det) && (st.win == 0 || (k.win & st.win))) {
                        if (f[q] < 0) f[q] = i;
                        l[q] = i;
                        cnt[q] += 1;
                        msum[q] += mi;
                    }
                }
            }
            const int slot = k.slot < 0 ? cfg.cls.n_slots : k.slot;
            const int row = hist_slot[slot];
            if (row >= 0) hist_in[row][mi] += 1;
            const int bin = mi / b;
            if (bin < n_bins) {
                for (size_t g = 0; g < scratch.size(); ++g) {
                    const int role = cfg.group_role[g][slot];
                    if (role >= 0) scratch[g][static_cast<size_t>(role) * n_bins + bin] += 1;
                }
            }
        }
        if (cfg.features) {
            for (size_t q = 0; q < ns; ++q) {
                StreamCols &c = scols[q];
                c.first.push_back(f[q]);
                c.last.push_back(l[q]);
                c.n.push_back(cnt[q]);
                if (cnt[q] > 0) {
                    const unsigned long long a = t.get_macro_time_at(static_cast<size_t>(f[q]));
                    const unsigned long long z = t.get_macro_time_at(static_cast<size_t>(l[q]));
                    const double d = static_cast<double>(z - a) * res_f * 1e3;
                    c.dur.push_back(d);
                    c.mean_macro.push_back((static_cast<double>(z + a) / 2.0) * res_f * 1e3);
                    c.rate.push_back(d > 0 ? static_cast<double>(cnt[q]) / d : kNaN);
                    c.mean_micro.push_back(micro_ns <= 0.0 ? -1.0
                                           : static_cast<double>(msum[q]) / static_cast<double>(cnt[q]) * micro_ns);
                } else {
                    c.dur.push_back(-1.0);
                    c.mean_macro.push_back(-1.0);
                    c.rate.push_back(-1.0);
                    c.mean_micro.push_back(-1.0);
                }
            }
        }
        for (size_t g = 0; g < scratch.size(); ++g) {
            auto &grp = res.groups_[g];
            auto &sc = scratch[g];
            for (int c = 0; c < 2 * n_bins; ++c) {
                if (sc[c]) {
                    grp.bins.push_back(c);
                    grp.counts.push_back(static_cast<int>(sc[c]));
                    grp.sum[c] += sc[c];
                    sc[c] = 0;
                }
            }
            grp.indptr.push_back(static_cast<long long>(grp.bins.size()));
        }
        bursts_done.store(static_cast<long long>(s_first.size()));
        maybe_submit(false);
    }

    void maybe_submit(bool all) {
        if (pool == nullptr) return;
        const long long have = static_cast<long long>(s_first.size());
        while (have - submitted >= batch_rows || (all && have > submitted)) {
            auto bt = std::make_shared<FitBatch>();
            bt->row0 = submitted;
            bt->n = std::min(batch_rows, have - submitted);
            for (const FitEntry &e : *entries) {
                const auto &grp = res.groups_[e.group];
                const long long k0 = grp.indptr[bt->row0], k1 = grp.indptr[bt->row0 + bt->n];
                std::vector<long long> ip(grp.indptr.begin() + bt->row0, grp.indptr.begin() + bt->row0 + bt->n + 1);
                for (auto &v : ip) v -= k0;
                bt->own_indptr.push_back(std::move(ip));
                bt->own_bins.emplace_back(grp.bins.begin() + k0, grp.bins.begin() + k1);
                bt->own_counts.emplace_back(grp.counts.begin() + k0, grp.counts.begin() + k1);
            }
            for (size_t k = 0; k < entries->size(); ++k)
                bt->views.push_back({bt->own_indptr[k].data(), bt->own_bins[k].data(), bt->own_counts[k].data()});
            submitted += bt->n;
            pool->submit(bt);
        }
    }

    void queue_search_burst(long long a, long long b) {
        ++n_search_bursts;
        search_bursts.emplace_back(a, b);
    }

    // ---- the searches -----------------------------------------------------------------

    /*! `TTTR::burst_search_cusum_sprt`, on the subset, pulling it through the cursor. */
    void cusum_sprt() {
        const double macro_res_ms = res_s * 1000.0;
        double IB, I0, I1;
        double background_cps = cfg.background_cps;
        double sbr = cfg.sbr;
        if (background_cps <= 0 || sbr <= 0) {
            // the estimates read the whole subset; a separate pass keeps the buffers bounded
            std::vector<double> rates;
            double bin_t = 0.0, bin_t2 = 0.0, I0max = 0.0;
            int bin_count = 0;
            long long i = 0;
            unsigned long long prev = 0;
            for (long long k = 0; k < N; ++k) {
                const Combo &c = cfg.cls.combos[cfg.cls.code(t.get_routing_channel_at(k), t.get_micro_time_at(k))];
                if (!c.subset) continue;
                const unsigned long long m = t.get_macro_time_at(k);
                if (i >= 1) {
                    const double d = (m - prev) * macro_res_ms;
                    bin_t += d; bin_count++;
                    if (bin_count >= 100) { if (bin_t > 0) rates.push_back(bin_count / bin_t); bin_t = 0.0; bin_count = 0; }
                    if (i > 1) bin_t2 += d;
                    if (i % 30 == 0) { const double tmp = 30 / bin_t2; if (tmp > I0max) I0max = tmp; bin_t2 = 0.0; }
                }
                prev = m;
                ++i;
            }
            if (i == 0) return;
            if (background_cps <= 0) {
                if (!rates.empty()) {
                    std::sort(rates.begin(), rates.end());
                    IB = rates[rates.size() / 2];
                    background_cps = IB * 1000.0;
                } else { IB = 0.001; background_cps = 1.0; }
            } else {
                IB = background_cps / 1000.0;
            }
            if (sbr <= 0) {
                I0 = I0max;
                I1 = I0 / std::exp(2.0) + IB;
                sbr = (I0 + IB) / IB;
            } else {
                I0 = (sbr - 1.0) * IB;
                I1 = I0 + IB;
            }
        } else {
            IB = background_cps / 1000.0;
            I0 = (sbr - 1.0) * IB;
            I1 = I0 + IB;
        }
        const double alpha = cfg.alpha, beta = cfg.beta;
        const double KL_disc = (IB - I1) / I1 + std::log(I1 / IB);
        const double A = (1.0 - beta) / alpha;
        const double B = beta / (1.0 - alpha);
        const double hA = std::log(A) / (I1 - IB);
        const double hB = std::log(B) / (I1 - IB);
        const double hC = std::log(I1 / IB) / (I1 - IB);
        const double tmp = alpha / 3.0 / (KL_disc + 1.0) / (KL_disc + 1.0) * std::log(1.0 / alpha);
        const double h = -std::log(tmp);
        const double Sa = std::log(I1 / IB);
        const double Sb = (I1 - IB);
        const long long nd = static_cast<long long>(std::round(std::log(1.0 / alpha) / KL_disc));

        if (!sub_has(0)) return;   // empty subset
        auto dt = [&](long long j) -> double {
            if (j == 0) return 0.0;
            return static_cast<double>(sub_t(j) - sub_t(j - 1)) * macro_res_ms;
        };
        // f == kEnd: "to the end of the subset", which the original knows as N
        auto cusum = [&](long long i, long long f) -> long long {
            const long long dj = (i < f) ? 1 : -1;
            long long j = i, k = f;
            double Sn = 0.0;
            while (j != f) {
                if (f == kEnd && !sub_has(j)) break;
                Sn += Sa - dt(j) * Sb;
                if (Sn < 0) Sn = 0.0;
                else if (Sn >= h) { k = j; break; }
                j += dj;
            }
            return k;
        };
        auto sprt = [&](long long i) -> long long {
            long long j = i, f = kEnd, n = 0;
            double Sn = 0.0;
            while (sub_has(j)) {
                Sn += dt(j);
                n++;
                if (Sn <= (static_cast<double>(n) * hC - hA)) { Sn = 0.0; n = 0; }
                else if (Sn > (static_cast<double>(n) * hC - hB)) { f = j; break; }
                j++;
            }
            return f;
        };
        long long kl = cusum(1, kEnd);
        while (kl != kEnd) {
            select_to(sub_index(kl));
            sub_keep_from(kl - nd - 2);
            const long long krp = sprt(kl);
            if (krp == kEnd) break;
            const long long kl1 = cusum(krp, kEnd);
            if (kl1 == kEnd) break;
            const long long kr = (kl1 > nd) ? cusum(kl1 - nd, kl) : cusum(0, kl);
            if (kr >= kl) {
                const long long burst_size = kr - kl + 1;
                if (burst_size >= cfg.cusum_min_photons) queue_search_burst(sub_index(kl), sub_index(kr));
            }
            kl = kl1;
            if (cancel.load()) break;
        }
        while (advance()) {}
    }

    /*! `TTTR::burst_search_sliding_window`, on the subset. */
    void sliding_window() {
        const int m = cfg.sw_m, L = cfg.sw_L;
        const unsigned long long Ti = static_cast<unsigned long long>(
            static_cast<long long>(cfg.sw_T / res_s));
        bool in_burst = false;
        long long i_start = 0;
        long long i = 0;
        for (; sub_has(i + m - 1); ++i) {
            if (sub_t(i + m - 1) - sub_t(i) <= Ti) {
                if (!in_burst) { in_burst = true; i_start = i; }
            } else if (in_burst) {
                in_burst = false;
                const long long i_stop = i + m - 2;
                if (i_stop - i_start + 1 >= L) queue_search_burst(sub_index(i_start), sub_index(i_stop));
            }
            if (!in_burst) {
                select_to(sub_index(i));
                sub_keep_from(i - 1);
            }
            if (cancel.load()) break;
        }
        while (advance()) {}
        const long long n_sub = sub_base + static_cast<long long>(sub_macro.size());
        if (n_sub < m) return;   // the original returns nothing for a subset shorter than m
        if (in_burst) {
            const long long i_stop = n_sub - 1;
            if (i_stop - i_start + 1 >= L) queue_search_burst(sub_index(i_start), sub_index(i_stop));
        }
    }

    void finish_selection() {
        select_to(N);
        if (have_run) finalize();
    }

    void store() {
        res.n_bursts_ = static_cast<long long>(s_first.size());
        res.i64_["first_photon"] = s_first;
        res.i64_["last_photon"] = s_last;
        res.i64_["n_photons"] = s_n;
        res.f64_["duration_ms"] = s_dur;
        res.f64_["mean_macro_time_ms"] = s_mean_macro;
        res.f64_["count_rate_khz"] = s_rate;
        if (cfg.features) {
            for (size_t q = 0; q < streams.size(); ++q) {
                const std::string sfx = "|" + stream_names[q];
                StreamCols &c = scols[q];
                res.i64_["first_photon" + sfx] = std::move(c.first);
                res.i64_["last_photon" + sfx] = std::move(c.last);
                res.i64_["n_photons" + sfx] = std::move(c.n);
                res.f64_["duration_ms" + sfx] = std::move(c.dur);
                res.f64_["mean_macro_time_ms" + sfx] = std::move(c.mean_macro);
                res.f64_["count_rate_khz" + sfx] = std::move(c.rate);
                res.f64_["mean_micro_time_ns" + sfx] = std::move(c.mean_micro);
            }
        }
        res.hist_channels_ = cfg.hist_channels;
        size_t len = 0;
        for (const auto &v : hist_all) len = std::max(len, v.size());
        res.hist_length_ = static_cast<int>(len);
        res.outside_hist_.assign(hist_all.size() * len, 0);
        for (size_t r = 0; r < hist_all.size(); ++r)
            for (size_t k = 0; k < hist_all[r].size(); ++k)
                res.outside_hist_[r * len + k] = hist_all[r][k] - hist_in[r][k];
    }
};

}  // namespace

// =======================================================================================
// BurstPipeline
// =======================================================================================

BurstPipeline::BurstPipeline(const std::string &config) : config_(config), impl_(std::make_shared<Impl>()) {
    impl_->parse(json::parse(config));
}

namespace {

std::vector<FitEntry> parse_fits(const json &fits, const BurstPipelineResult &res,
                                 const std::map<int, std::vector<double>> &bgs,
                                 const std::map<int, int> &bg_rows) {
    std::vector<FitEntry> entries;
    int k = 0;
    for (const auto &f : fits) {
        FitEntry e = fit_entry_from(f, res);
        auto it = bgs.find(k);
        if (it != bgs.end()) {
            if (it->second.size() != static_cast<size_t>(bg_rows.at(k)) * 2 * e.nf)
                throw std::invalid_argument("BurstPipeline: per-burst backgrounds of " + e.detector +
                                            " must have 2 * fitted bins columns");
            e.burst_backgrounds = it->second;
            e.burst_background_rows = bg_rows.at(k);
        }
        entries.push_back(std::move(e));
        ++k;
    }
    return entries;
}

std::string stats_of(const BurstPipelineResult &res, long long N, long long n_sub, long long n_search,
                     long long n_selected, long long peak_codes, long long peak_sub, bool cancelled,
                     const std::vector<std::pair<std::string, double>> &seconds) {
    json s;
    s["photons"] = N;
    s["subset_photons"] = n_sub;
    s["bursts"] = res.n_bursts_;
    s["search_bursts"] = n_search;
    s["selected_photons"] = n_selected;
    s["cancelled"] = cancelled;
    s["peak_buffered_photons"] = peak_codes;
    s["peak_buffered_subset"] = peak_sub;
    s["bytes_out"] = res.nbytes();
    json sec = json::object();
    for (const auto &kv : seconds) sec[kv.first] = kv.second;
    s["seconds"] = sec;
    return s.dump();
}

}  // namespace

std::shared_ptr<BurstPipelineResult> BurstPipeline::run(std::shared_ptr<TTTR> tttr) {
    calls_.fetch_add(1);
    if (!tttr) throw std::invalid_argument("BurstPipeline.run: no TTTR");
    cancel_.store(false);
    photons_done_.store(0);
    bursts_done_.store(0);
    fits_done_.store(0);
    auto res = std::make_shared<BurstPipelineResult>();
    Run r(*impl_, *tttr, *res, photons_done_, bursts_done_, cancel_);
    int n_bins = impl_->n_bins;
    TTTRHeader *h = tttr->get_header();
    if (n_bins <= 0) {
        const int nm = h ? static_cast<int>(h->get_number_of_micro_time_channels()) : 1;
        n_bins = std::max(1, (nm + impl_->binning - 1) / impl_->binning);
    }
    r.init(n_bins);
    photons_total_.store(r.N);
    const double t_start = now_s();

    std::vector<FitEntry> entries;
    std::unique_ptr<FitPool> pool;
    if (!impl_->fits.empty()) {
        entries = parse_fits(impl_->fits, *res, impl_->burst_backgrounds, impl_->burst_background_rows);
        for (const auto &e : entries) {
            if (e.tail && impl_->fits[&e - entries.data()].value("tail_start", -1) < 0)
                throw std::invalid_argument("BurstPipeline.run: a fused tail fit needs tail_start "
                                            "(the summed decay is not known while the bursts stream)");
        }
        pool.reset(new FitPool(entries, thread_count(impl_->threads), fits_done_, cancel_));
        r.pool = pool.get();
        r.entries = &entries;
    }

    if (!impl_->filter_active) {
        // no search: the mask is the pre-filter and the delta-T interval alone
        r.finish_selection();
    } else if (impl_->algorithm == "cusum_sprt") {
        r.cusum_sprt();
        r.finish_selection();
    } else {
        r.sliding_window();
        r.finish_selection();
    }
    while (r.advance()) {}
    const double t_pass = now_s();
    const long long n_sub = r.sub_base + static_cast<long long>(r.sub_macro.size());

    if (pool) {
        r.maybe_submit(true);
        pool->finish();
        if (!pool->error().empty()) throw std::runtime_error("BurstPipeline.run: fit failed: " + pool->error());
    }
    const double t_fit = now_s();
    r.store();
    if (impl_->confidence && res->n_bursts_ > 0) {
        std::vector<long long> flat;
        flat.reserve(static_cast<size_t>(2 * res->n_bursts_));
        for (long long b = 0; b < res->n_bursts_; ++b) { flat.push_back(r.s_first[b]); flat.push_back(r.s_last[b]); }
        res->f64_["confidence_sigma"] = tttr->burst_confidence(flat);
    } else {
        res->f64_["confidence_sigma"].assign(static_cast<size_t>(res->n_bursts_), 0.0);
    }
    if (pool) store_fit_columns(*res, entries, pool->gather(res->n_bursts_));
    res->cancelled_ = cancel_.load();
    res->stats_json_ = stats_of(*res, r.N, n_sub, r.n_search_bursts, r.selected_photons, r.peak_codes, r.peak_sub, res->cancelled_,
                                {{"pass", t_pass - t_start}, {"fit_tail", t_fit - t_pass},
                                 {"store", now_s() - t_fit}});
    return res;
}

std::shared_ptr<BurstPipelineResult> BurstPipeline::decays(std::shared_ptr<TTTR> tttr,
                                                           long long *first, int n_first,
                                                           long long *last, int n_last,
                                                           int binning, int n_bins) {
    calls_.fetch_add(1);
    if (!tttr) throw std::invalid_argument("BurstPipeline.decays: no TTTR");
    if (n_first != n_last) throw std::invalid_argument("BurstPipeline.decays: first and last differ in length");
    cancel_.store(false);
    photons_done_.store(0);
    bursts_done_.store(0);
    Impl cfg = *impl_;
    if (binning > 0) cfg.binning = binning;
    if (n_bins > 0) cfg.n_bins = n_bins;
    auto res = std::make_shared<BurstPipelineResult>();
    Run r(cfg, *tttr, *res, photons_done_, bursts_done_, cancel_);
    int nb = cfg.n_bins;
    if (nb <= 0) {
        TTTRHeader *h = tttr->get_header();
        const int nm = h ? static_cast<int>(h->get_number_of_micro_time_channels()) : 1;
        nb = std::max(1, (nm + cfg.binning - 1) / cfg.binning);
    }
    r.init(nb);
    photons_total_.store(r.N);
    const double t0 = now_s();
    // bursts in table order; the cursor only moves forward, so classify everything up
    // to the furthest burst first when the table is not sorted
    bool sorted = true;
    for (int b = 1; b < n_first; ++b) if (first[b] < first[b - 1]) { sorted = false; break; }
    long long far = 0;
    for (int b = 0; b < n_last; ++b) far = std::max(far, last[b]);
    for (int b = 0; b < n_first; ++b) {
        const long long s = std::max<long long>(0, first[b]);
        const long long e = std::min<long long>(r.N - 1, last[b]);
        if (cancel_.load()) break;
        if (e < s) {
            // an empty or unreadable burst keeps its row
            r.s_first.push_back(first[b]); r.s_last.push_back(last[b]); r.s_n.push_back(0);
            r.s_dur.push_back(kNaN); r.s_mean_macro.push_back(kNaN); r.s_rate.push_back(kNaN);
            for (auto &c : r.scols) {
                c.first.push_back(-1); c.last.push_back(-1); c.n.push_back(0);
                c.dur.push_back(-1); c.mean_macro.push_back(-1); c.rate.push_back(-1); c.mean_micro.push_back(-1);
            }
            for (auto &g : res->groups_) g.indptr.push_back(static_cast<long long>(g.bins.size()));
            continue;
        }
        r.ensure_codes(sorted ? e : far);
        r.emit(s, e);
        if (sorted) {
            // the codes before this burst are no longer needed
            r.next = s;
            r.trim_codes();
        }
    }
    while (r.advance()) {}
    r.store();
    res->f64_["confidence_sigma"].assign(static_cast<size_t>(res->n_bursts_), 0.0);
    res->cancelled_ = cancel_.load();
    res->stats_json_ = stats_of(*res, r.N, 0, 0, 0, r.peak_codes, 0, res->cancelled_, {{"pass", now_s() - t0}});
    return res;
}

void BurstPipeline::set_burst_backgrounds(int fit_index, double *backgrounds, int n_rows, int n_cols) {
    calls_.fetch_add(1);
    impl_->burst_backgrounds[fit_index].assign(backgrounds, backgrounds + static_cast<size_t>(n_rows) * n_cols);
    impl_->burst_background_rows[fit_index] = n_rows;
}

void BurstPipeline::fit(std::shared_ptr<BurstPipelineResult> result, const std::string &config) {
    calls_.fetch_add(1);
    if (!result) throw std::invalid_argument("BurstPipeline.fit: no result");
    cancel_.store(false);
    fits_done_.store(0);
    const json j = json::parse(config);
    const double t0 = now_s();
    std::vector<FitEntry> entries = parse_fits(j.value("fits", json::array()), *result,
                                               impl_->burst_backgrounds, impl_->burst_background_rows);
    std::vector<FitOutputs> outputs;
    const long long rows = result->n_bursts_;
    {
        FitPool pool(entries, thread_count(j.value("n_threads", impl_->threads)), fits_done_, cancel_);
        const long long batch = std::max<long long>(16, j.value("batch_rows", 256LL));
        for (long long r0 = 0; r0 < rows; r0 += batch) {
            auto bt = std::make_shared<FitBatch>();
            bt->row0 = r0;
            bt->n = std::min(batch, rows - r0);
            for (const FitEntry &e : entries) {
                const auto &grp = result->groups_[e.group];
                // views into the result's CSR: rows are addressed relative to row0
                bt->views.push_back({grp.indptr.data() + r0, grp.bins.data(), grp.counts.data()});
            }
            pool.submit(bt);
        }
        pool.finish();
        if (!pool.error().empty()) throw std::runtime_error("BurstPipeline.fit: " + pool.error());
        outputs = pool.gather(rows);
    }
    store_fit_columns(*result, entries, outputs);
    json s = json::parse(result->stats_json_);
    s["seconds"]["fit"] = now_s() - t0;
    s["fitted_detectors"] = static_cast<int>(entries.size());
    s["bytes_out"] = result->nbytes();
    result->stats_json_ = s.dump();
}

// =======================================================================================
// StrictBackground
// =======================================================================================

namespace {

/*! NumPy's `_lerp`, which the default quantile method uses. */
inline double lerp(double a, double b, double t) {
    const double diff = b - a;
    return t >= 0.5 ? b - diff * (1.0 - t) : a + diff * t;
}

/*! `_gamma_quantile`: bisection on the Erlang CDF. */
double gamma_quantile(int shape, double q) {
    auto cdf = [shape](double x) {
        double s = 0.0, fact = 1.0;
        for (int j = 0; j < shape; ++j) {
            if (j > 0) fact *= j;
            s += std::exp(-x) * std::pow(x, j) / fact;
        }
        return 1.0 - s;
    };
    double lo = 0.0, hi = static_cast<double>(shape) * 10.0 + 10.0;
    for (int i = 0; i < 100; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (cdf(mid) < q) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

constexpr size_t kDenseGaps = 1u << 20;   // gaps up to 1 M ticks counted densely, longer ones listed

/*! Union of closed intervals, sorted and disjoint (`_merged`). */
void merged(std::vector<std::pair<double, double>> &iv) {
    if (iv.empty()) return;
    std::stable_sort(iv.begin(), iv.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> out;
    double lo = iv[0].first, hi = iv[0].second;
    for (size_t k = 1; k < iv.size(); ++k) {
        if (iv[k].first > hi) { out.emplace_back(lo, hi); lo = iv[k].first; hi = iv[k].second; }
        else hi = std::max(hi, iv[k].second);
    }
    out.emplace_back(lo, hi);
    iv.swap(out);
}

}  // namespace

StrictBackground::StrictBackground(std::shared_ptr<TTTR> tttr, long long *first, int n_first,
                                   long long *last, int n_last, const std::string &config)
    : tttr_(std::move(tttr)) {
    if (!tttr_) throw std::invalid_argument("StrictBackground: no TTTR");
    if (n_first != n_last) throw std::invalid_argument("StrictBackground: first and last differ in length");
    const json j = json::parse(config);
    TTTR &t = *tttr_;
    TTTRHeader *h = t.get_header();
    res_ = j.value("macro_time_resolution", 0.0);
    if (!(res_ > 0)) res_ = h ? h->get_macro_time_resolution() : 1.0;
    if (!(res_ > 0)) res_ = 1.0;
    m_ = std::max(2, j.value("photon_window", 10));
    iterations_ = std::max(1, j.value("iterations", 4));
    rate_khz_given_ = j.value("rate_khz", 0.0);
    factor_ = (m_ - 1) / gamma_quantile(m_ - 1, j.value("false_alarm", 0.01));
    const double margin_ms = j.value("margin_ms", 2.0);
    const double margin_fraction = j.value("margin_fraction", 0.5);
    const std::vector<int> chs = int_list(j.value("channels", json::array()));
    channel_sel_.assign(256, chs.empty() ? 1 : 0);
    for (int c : chs) channel_sel_[static_cast<unsigned char>(static_cast<signed char>(c))] = 1;
    hist_channels_ = int_list(j.value("histogram_channels", json::array()));
    hist_slot_.assign(256, -1);
    for (size_t k = 0; k < hist_channels_.size(); ++k)
        hist_slot_[static_cast<unsigned char>(static_cast<signed char>(hist_channels_[k]))] = static_cast<int>(k);
    hist_length_ = std::max(1, h ? static_cast<int>(h->get_number_of_micro_time_channels()) : 1);
    const long long N = static_cast<long long>(t.size());
    if (N > 0) {
        t0_ = static_cast<double>(t.get_macro_time_at(0)) * res_;
        t1_ = static_cast<double>(t.get_macro_time_at(static_cast<size_t>(N - 1))) * res_;
        for (long long i = 0; i < N; ++i) hist_length_ = std::max<int>(hist_length_, t.get_micro_time_at(i) + 1);
    }
    total_ = N > 1 ? t1_ - t0_ : 0.0;
    // the bursts as contiguous runs of photons (bursts that touch are one run)
    std::vector<std::pair<long long, long long>> b;
    for (int k = 0; k < n_first; ++k) {
        const long long s = std::max<long long>(0, first[k]);
        const long long e = std::min<long long>(N - 1, last[k]);
        if (e >= s) b.emplace_back(s, e);
    }
    std::sort(b.begin(), b.end());
    std::vector<std::pair<long long, long long>> runs;
    for (const auto &p : b) {
        if (!runs.empty() && p.first <= runs.back().second + 1) runs.back().second = std::max(runs.back().second, p.second);
        else runs.push_back(p);
    }
    for (const auto &p : runs) {
        const double ts = static_cast<double>(t.get_macro_time_at(static_cast<size_t>(p.first))) * res_;
        const double te = static_cast<double>(t.get_macro_time_at(static_cast<size_t>(p.second))) * res_;
        const double pad = margin_ms * 1e-3 + margin_fraction * (te - ts);
        flank_lo_.push_back(ts - pad);
        flank_hi_.push_back(te + pad);
    }
    gap_hist_dense_.assign(kDenseGaps, 0);
}

double StrictBackground::tail_rate_khz() const {
    // `_poisson_tail_rate(gaps) / 1e3` from the gap histogram
    if (n_gaps_ == 0) return 0.0;
    const double mean = gap_sum_ / static_cast<double>(n_gaps_);
    if (n_gaps_ < 20) return 1.0 / std::max(mean, 1e-12) / 1e3;
    std::vector<unsigned long long> over = gap_overflow_;
    std::sort(over.begin(), over.end());
    // the 80 order statistics np.quantile interpolates between, found in one sweep
    double gam[40], y[40];
    std::vector<std::pair<long long, int>> need;   // (rank, slot): slot 2i = lower, 2i+1 = upper
    for (int i = 0; i < 40; ++i) {
        const double q = 0.5 + (0.995 - 0.5) * i / 39.0;
        const double hh = (static_cast<double>(n_gaps_) - 1.0) * q;
        const long long lo = static_cast<long long>(std::floor(hh));
        gam[i] = hh - static_cast<double>(lo);
        y[i] = std::log(1.0 - q);
        need.emplace_back(lo, 2 * i);
        need.emplace_back(std::min(lo + 1, n_gaps_ - 1), 2 * i + 1);
    }
    std::sort(need.begin(), need.end());
    double value[80];
    size_t q = 0;
    long long acc = 0;
    for (size_t v = 0; v < gap_hist_dense_.size() && q < need.size(); ++v) {
        acc += gap_hist_dense_[v];
        while (q < need.size() && need[q].first < acc) { value[need[q].second] = static_cast<double>(v) * res_; ++q; }
    }
    for (; q < need.size(); ++q)
        value[need[q].second] = static_cast<double>(over[static_cast<size_t>(need[q].first - acc)]) * res_;
    double x[40];
    for (int i = 0; i < 40; ++i) x[i] = lerp(value[2 * i], value[2 * i + 1], gam[i]);
    double mx = 0, my = 0;
    for (int i = 0; i < 40; ++i) { mx += x[i]; my += y[i]; }
    mx /= 40; my /= 40;
    double sxy = 0, sxx = 0;
    for (int i = 0; i < 40; ++i) { sxy += (x[i] - mx) * (y[i] - my); sxx += (x[i] - mx) * (x[i] - mx); }
    const double slope = sxx > 0 ? sxy / sxx : 0.0;
    const double rate = slope < 0 ? -slope : 1.0 / std::max(mean, 1e-12);
    return rate / 1e3;
}

/*!
 * keep_slow(threshold) followed by the inner-gap statistics of gap_rate: pass A
 * finds the merged fast local-rate windows of the selected photons, pass B tests
 * every photon against those and the flanks.
 */
void StrictBackground::run_keep(double threshold_khz, bool histogram) {
    TTTR &t = *tttr_;
    const long long N = static_cast<long long>(t.size());
    std::vector<std::pair<double, double>> iv;
    for (size_t k = 0; k < flank_lo_.size(); ++k) iv.emplace_back(flank_lo_[k], flank_hi_[k]);
    {
        std::vector<double> ring(static_cast<size_t>(m_), 0.0);
        long long ns = 0;
        bool open = false;
        double lo = 0, hi = 0;
        for (long long i = 0; i < N; ++i) {
            if (!channel_sel_[static_cast<unsigned char>(t.get_routing_channel_at(i))]) continue;
            const double ti = static_cast<double>(t.get_macro_time_at(i)) * res_;
            ring[static_cast<size_t>(ns % m_)] = ti;
            ++ns;
            if (ns < m_) continue;
            const double t_first = ring[static_cast<size_t>((ns - m_) % m_)];
            const double span = ti - t_first;
            if (!(span > 0.0)) continue;
            const double local = (m_ - 1) / span / 1e3;
            if (!(local > threshold_khz)) continue;
            if (open && !(t_first > hi)) hi = ti;
            else { if (open) iv.emplace_back(lo, hi); lo = t_first; hi = ti; open = true; }
        }
        if (open) iv.emplace_back(lo, hi);
    }
    merged(iv);
    double cut = 0.0;
    for (const auto &p : iv) cut += std::min(std::max(p.second, t0_), t1_) - std::min(std::max(p.first, t0_), t1_);
    seconds_ = std::max(total_ - cut, 0.0);

    std::fill(gap_hist_dense_.begin(), gap_hist_dense_.end(), 0);
    gap_overflow_.clear();
    n_gaps_ = 0;
    gap_sum_ = 0.0;
    kept_ = kept_sel_ = 0;
    if (histogram) histogram_.assign(hist_channels_.size() * static_cast<size_t>(hist_length_), 0);
    size_t k = 0;
    bool prev_kept = false, have_prev = false;
    unsigned long long prev_m = 0;
    double prev_t = 0.0;
    for (long long i = 0; i < N; ++i) {
        const unsigned long long mi = t.get_macro_time_at(i);
        const double ti = static_cast<double>(mi) * res_;
        while (k < iv.size() && iv[k].second < ti) ++k;
        const bool kept = !(k < iv.size() && iv[k].first <= ti);
        const signed char ch = t.get_routing_channel_at(i);
        if (kept) {
            ++kept_;
            if (histogram) {
                const int row = hist_slot_[static_cast<unsigned char>(ch)];
                if (row >= 0) histogram_[static_cast<size_t>(row) * hist_length_ + t.get_micro_time_at(i)] += 1;
            }
        }
        if (!channel_sel_[static_cast<unsigned char>(ch)]) continue;
        if (kept) ++kept_sel_;
        if (have_prev && kept && prev_kept) {
            const unsigned long long d = mi - prev_m;
            if (d < kDenseGaps) gap_hist_dense_[d] += 1; else gap_overflow_.push_back(d);
            gap_sum_ += ti - prev_t;
            ++n_gaps_;
        }
        have_prev = true;
        prev_kept = kept;
        prev_m = mi;
        prev_t = ti;
    }
}

double StrictBackground::gap_rate(double fallback) const {
    return n_gaps_ >= 20 ? tail_rate_khz() : fallback;
}

double StrictBackground::rate_scale_at(double rate_khz) const {
    const double counted = seconds_ > 0 ? static_cast<double>(kept_sel_) / seconds_ / 1e3 : 0.0;
    return counted > 0 ? rate_khz / counted : 1.0;
}

StrictBackgroundResult StrictBackground::snapshot(double rate, double threshold, int iterations) const {
    StrictBackgroundResult r;
    r.seconds = seconds_;
    r.total_seconds = total_;
    r.rate_khz = rate;
    r.threshold_khz = threshold;
    r.photons = kept_;
    r.selected_photons = kept_sel_;
    r.iterations = iterations;
    r.rate_scale = rate_scale_at(rate);
    r.inner_gaps = n_gaps_;
    r.channels = hist_channels_;
    r.length = hist_length_;
    r.histogram = histogram_;
    return r;
}

StrictBackgroundResult StrictBackground::select() {
    double rate;
    if (rate_khz_given_ > 0) {
        rate = rate_khz_given_;
    } else {
        // the gaps between consecutive selected photons outside the flanks
        TTTR &t = *tttr_;
        const long long N = static_cast<long long>(t.size());
        std::vector<std::pair<double, double>> iv;
        for (size_t k = 0; k < flank_lo_.size(); ++k) iv.emplace_back(flank_lo_[k], flank_hi_[k]);
        merged(iv);
        std::fill(gap_hist_dense_.begin(), gap_hist_dense_.end(), 0);
        gap_overflow_.clear();
        n_gaps_ = 0; gap_sum_ = 0.0;
        size_t k = 0;
        bool have = false;
        unsigned long long pm = 0;
        double pt = 0.0;
        for (long long i = 0; i < N; ++i) {
            if (!channel_sel_[static_cast<unsigned char>(t.get_routing_channel_at(i))]) continue;
            const unsigned long long mi = t.get_macro_time_at(i);
            const double ti = static_cast<double>(mi) * res_;
            while (k < iv.size() && iv[k].second < ti) ++k;
            if (k < iv.size() && iv[k].first <= ti) continue;
            if (have) {
                const unsigned long long d = mi - pm;
                if (d < kDenseGaps) gap_hist_dense_[d] += 1; else gap_overflow_.push_back(d);
                gap_sum_ += ti - pt;
                ++n_gaps_;
            }
            have = true; pm = mi; pt = ti;
        }
        rate = tail_rate_khz();
    }
    double threshold = factor_ * rate;
    int done = 0;
    for (done = 1; done <= iterations_; ++done) {
        threshold = factor_ * rate;
        run_keep(threshold, true);
        const double nr = gap_rate(rate);
        const bool converged = std::fabs(nr - rate) <= 0.01 * std::max(rate, 1e-12);
        rate = nr;
        if (converged) break;
    }
    if (done > iterations_) done = iterations_;
    return snapshot(rate, threshold, done);
}

StrictBackgroundResult StrictBackground::keep_at(double threshold_khz) {
    run_keep(threshold_khz, true);
    return snapshot(gap_rate(threshold_khz / factor_), threshold_khz, 0);
}

}  // namespace tttrlib
