// SPDX-License-Identifier: BSD-3-Clause
//
// This file is the streaming module's SWIG interface, and it lives with the
// module like every other one. Until 2026-08-11 a second, DIVERGENT copy sat
// in `ext/python/Streaming.i`, left behind by the module relocation
// (`ac3cadda2`) -- and that copy was the one SWIG compiled, because
// `%include "Streaming.i"` resolves against the including file's own directory
// first. Edits made here did nothing at all, silently. The shadow copy is
// deleted; if you add one back, nothing here is compiled again.
%{
#include "StreamingCorrelator.h"
#include "StreamingCLSMImage.h"
#include "StreamingBurstDetector.h"
#include "StreamingDecayHistogram.h"
#include "StreamingIntensityTrace.h"
#include "StreamingBurstAnalyzer.h"
%}

// Map exception translations.
%exception {
    try {
        $action
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    }
}

// A chunk crosses the language boundary ONCE.
//
// The consumers here take (pointer, n) and loop in C++; the Python `push_np`
// helpers below used to loop in *Python* over `push_photon`, which measured
// 1.13 us/photon -- 30x a numpy histogram of the same photons, and enough to
// eat half a core on a 100 kHz live acquisition. The array overloads existed
// and were simply not reachable from Python: no numpy typemap mapped an
// ndarray onto their pointer arguments, so `push_photons(array, None, n)`
// raised.
//
// These typemaps + the `push_arrays` methods below make one chunk one call.
// The parameter names are deliberately distinctive: %apply is global and keyed
// by name, so a generic `(double* w, int n)` here would silently rewrite
// unrelated signatures in other modules.
%apply (unsigned long long* IN_ARRAY1, int DIM1) {(unsigned long long* st_macro_times, int st_n_macro_times)}
%apply (double* IN_ARRAY1, int DIM1)             {(double* st_weights, int st_n_weights)}
%apply (int* IN_ARRAY1, int DIM1)                {(int* st_channels, int st_n_channels)}
%apply (unsigned short* IN_ARRAY1, int DIM1)     {(unsigned short* st_microtimes, int st_n_microtimes)}

// SWIG doesn't parse C++ default arguments reliably; add explicit overloads
// before %include so Python callers can omit the weight.
%extend tttrlib::StreamingCorrelator {
    void push_photon(uint64_t macro_time) {
        $self->push_photon(macro_time, 1.0);
    }
    void push_photon(uint64_t macro_time, int channel) {
        $self->push_photon(macro_time, 1.0, channel);
    }
}
%extend tttrlib::StreamingDecayHistogram {
    void push_photon(uint16_t microtime) {
        $self->push_photon(microtime, 0, 1.0);
    }
    void push_photon(uint16_t microtime, int channel) {
        $self->push_photon(microtime, channel, 1.0);
    }
}
%extend tttrlib::StreamingPhasor {
    void push_photon(uint16_t microtime) {
        $self->push_photon(microtime, 1.0);
    }
}
%extend tttrlib::StreamingIntensityTrace {
    void push_photon(uint64_t macro_time) {
        $self->push_photon(macro_time, 1.0);
    }
}

%include "StreamingCLSMImage.h"
%include "StreamingCorrelator.h"
%include "StreamingBurstDetector.h"
%include "StreamingDecayHistogram.h"
%include "StreamingIntensityTrace.h"
%include "StreamingBurstAnalyzer.h"

%extend tttrlib::StreamingCorrelator {
    // One chunk, one call. `st_n_weights == 0` means unit weights and
    // `st_n_channels == 0` means the autocorrelation (both channels fed).
    void push_arrays(
        unsigned long long* st_macro_times, int st_n_macro_times,
        double* st_weights, int st_n_weights,
        int* st_channels, int st_n_channels
    ) {
        if (st_n_weights != 0 && st_n_weights != st_n_macro_times)
            throw std::invalid_argument("push_arrays: weights and macro times differ in length");
        if (st_n_channels != 0 && st_n_channels != st_n_macro_times)
            throw std::invalid_argument("push_arrays: channels and macro times differ in length");
        const uint64_t* mt = reinterpret_cast<const uint64_t*>(st_macro_times);
        const double* w = st_n_weights ? st_weights : nullptr;
        if (st_n_channels)
            $self->push_photons(mt, w, st_channels, st_n_macro_times);
        else
            $self->push_photons(mt, w, st_n_macro_times);
    }

// Only the %pythoncode below is Python-only. push_arrays above is portable
// C++ over the IN_ARRAY1 typemaps, which rarrays.i/jarrays.i/jsarrays.i all
// implement for these types -- fencing it made bulk push Python-only for no
// reason, leaving the other three languages with per-photon calls at ~1.13us
// each (BUGS 2026-08-11).
#ifdef SWIGPYTHON
    %pythoncode %{
    @property
    def x_axis(self):
        import numpy as np
        return np.array(self.get_x_axis(), dtype=float)

    @property
    def correlation(self):
        import numpy as np
        return np.array(self.get_correlation(), dtype=float)

    @property
    def correlation_normalized(self):
        import numpy as np
        return np.array(self.get_correlation_normalized(), dtype=float)

    def push_np(self, macro_times, weights=None, channels=None):
        """Push a whole chunk of photons in one call.

        `channels` selects the cross-correlation: channel 0 and channel 1 are
        the two correlated streams and the lag runs 0 -> 1, exactly as in the
        batch correlator. Omitting it feeds every photon to both channels --
        the autocorrelation.
        """
        import numpy as np
        mt = np.ascontiguousarray(macro_times, dtype=np.uint64)
        w = (np.empty(0, dtype=float) if weights is None
             else np.ascontiguousarray(weights, dtype=float))
        c = (np.empty(0, dtype=np.int32) if channels is None
             else np.ascontiguousarray(channels, dtype=np.int32))
        self.push_arrays(mt, w, c)

    def __repr__(self):
        return (f"StreamingCorrelator(n_bins={self.n_bins()}, "
                f"n_casc={self.n_casc()}, photons={self.photon_count()})")
    %}
#endif
}

%extend tttrlib::StreamingBurstDetector {
    void push_arrays(unsigned long long* st_macro_times, int st_n_macro_times) {
        $self->push_photons(
            reinterpret_cast<const uint64_t*>(st_macro_times), st_n_macro_times);
    }

// Only the %pythoncode below is Python-only. push_arrays above is portable
// C++ over the IN_ARRAY1 typemaps, which rarrays.i/jarrays.i/jsarrays.i all
// implement for these types -- fencing it made bulk push Python-only for no
// reason, leaving the other three languages with per-photon calls at ~1.13us
// each (BUGS 2026-08-11).
#ifdef SWIGPYTHON
    %pythoncode %{
    @property
    def bursts(self):
        return self.get_burst_indices()

    @property
    def burst_count(self):
        return len(self.get_burst_indices()) // 2

    def push_np(self, macro_times):
        import numpy as np
        self.push_arrays(np.ascontiguousarray(macro_times, dtype=np.uint64))
    %}
#endif
}

%extend tttrlib::StreamingDecayHistogram {
    // `st_n_channels == 0` puts every photon in channel 0.
    void push_arrays(
        unsigned short* st_microtimes, int st_n_microtimes,
        int* st_channels, int st_n_channels
    ) {
        if (st_n_channels != 0 && st_n_channels != st_n_microtimes)
            throw std::invalid_argument("push_arrays: channels and microtimes differ in length");
        $self->push_photons(
            reinterpret_cast<const uint16_t*>(st_microtimes),
            st_n_channels ? st_channels : nullptr,
            st_n_microtimes);
    }

// Only the %pythoncode below is Python-only. push_arrays above is portable
// C++ over the IN_ARRAY1 typemaps, which rarrays.i/jarrays.i/jsarrays.i all
// implement for these types -- fencing it made bulk push Python-only for no
// reason, leaving the other three languages with per-photon calls at ~1.13us
// each (BUGS 2026-08-11).
#ifdef SWIGPYTHON
    %pythoncode %{
    @property
    def histogram(self):
        import numpy as np
        nc = self.n_channels()
        nb = self.n_bins()
        return np.asarray(self.get_histogram(), dtype=float).reshape(nc, nb)

    def push_np(self, microtimes, channels=None):
        import numpy as np
        mt = np.ascontiguousarray(microtimes, dtype=np.uint16)
        ch = (np.empty(0, dtype=np.int32) if channels is None
              else np.ascontiguousarray(channels, dtype=np.int32))
        self.push_arrays(mt, ch)
    %}
#endif
}

%extend tttrlib::StreamingPhasor {
    void push_arrays(unsigned short* st_microtimes, int st_n_microtimes) {
        $self->push_photons(
            reinterpret_cast<const uint16_t*>(st_microtimes), st_n_microtimes);
    }

// Only the %pythoncode below is Python-only. push_arrays above is portable
// C++ over the IN_ARRAY1 typemaps, which rarrays.i/jarrays.i/jsarrays.i all
// implement for these types -- fencing it made bulk push Python-only for no
// reason, leaving the other three languages with per-photon calls at ~1.13us
// each (BUGS 2026-08-11).
#ifdef SWIGPYTHON
    %pythoncode %{
    @property
    def phasor(self):
        return self.get_phasor()

    def push_np(self, microtimes):
        import numpy as np
        self.push_arrays(np.ascontiguousarray(microtimes, dtype=np.uint16))
    %}
#endif
}

%extend tttrlib::StreamingIntensityTrace {
    // `st_n_weights == 0` counts photons, which is what the batch
    // compute_intensity_trace does.
    void push_arrays(
        unsigned long long* st_macro_times, int st_n_macro_times,
        double* st_weights, int st_n_weights
    ) {
        if (st_n_weights != 0 && st_n_weights != st_n_macro_times)
            throw std::invalid_argument("push_arrays: weights and macro times differ in length");
        $self->push_photons(
            reinterpret_cast<const uint64_t*>(st_macro_times),
            st_n_weights ? st_weights : nullptr,
            st_n_macro_times);
    }

// Only the %pythoncode below is Python-only. push_arrays above is portable
// C++ over the IN_ARRAY1 typemaps, which rarrays.i/jarrays.i/jsarrays.i all
// implement for these types -- fencing it made bulk push Python-only for no
// reason, leaving the other three languages with per-photon calls at ~1.13us
// each (BUGS 2026-08-11).
#ifdef SWIGPYTHON
    %pythoncode %{
    @property
    def y(self):
        import numpy as np
        return np.asarray(self.counts(), dtype=float)

    @property
    def x(self):
        """Bin-start times of the retained bins, in seconds."""
        import numpy as np
        return np.asarray(self.get_time_axis(), dtype=float)

    def push_np(self, macro_times, weights=None):
        import numpy as np
        mt = np.ascontiguousarray(macro_times, dtype=np.uint64)
        w = (np.empty(0, dtype=float) if weights is None
             else np.ascontiguousarray(weights, dtype=float))
        self.push_arrays(mt, w)

    def __repr__(self):
        return (f"StreamingIntensityTrace(bin_width={self.bin_width():.3e} s, "
                f"bins={self.n_bins()}, photons={self.photon_count()})")
    %}
#endif
}

%extend tttrlib::StreamingBurstAnalyzer {
    // One chunk, one call. `st_n_microtimes == 0` means micro time 0 and
    // `st_n_channels == 0` means routing channel 0 for every photon.
    void push_arrays(
        unsigned long long* st_macro_times, int st_n_macro_times,
        unsigned short* st_microtimes, int st_n_microtimes,
        int* st_channels, int st_n_channels
    ) {
        if (st_n_microtimes != 0 && st_n_microtimes != st_n_macro_times)
            throw std::invalid_argument("push_arrays: micro times and macro times differ in length");
        if (st_n_channels != 0 && st_n_channels != st_n_macro_times)
            throw std::invalid_argument("push_arrays: channels and macro times differ in length");
        $self->push_photons(
            reinterpret_cast<const uint64_t*>(st_macro_times),
            st_n_microtimes ? reinterpret_cast<const uint16_t*>(st_microtimes) : nullptr,
            st_n_channels ? st_channels : nullptr,
            st_n_macro_times);
    }

#ifdef SWIGPYTHON
    %pythoncode %{
    PRESETS = ("single_color", "two_color", "pie", "alex", "mfd", "three_color_pie")

    def push_np(self, macro_times, micro_times=None, channels=None):
        """Push a whole chunk of photons in one call into C++."""
        import numpy as np
        mt = np.ascontiguousarray(macro_times, dtype=np.uint64)
        mi = (np.empty(0, dtype=np.uint16) if micro_times is None
              else np.ascontiguousarray(micro_times, dtype=np.uint16))
        ch = (np.empty(0, dtype=np.int32) if channels is None
              else np.ascontiguousarray(channels, dtype=np.int32))
        self.push_arrays(mt, mi, ch)

    @classmethod
    def from_setup(cls, setup, preset="pie", *, min_photons=20, window_photons=10,
                   window_time=5e-4, macro_time_resolution=None, micro_time_resolution=None,
                   donor="green", acceptor="red", prompt="prompt", delay="delayed",
                   colors=("blue", "green", "red"), factors=None, subtract_background=False,
                   alex_period=None, alex_windows=None, search_channels=None,
                   histograms=True, trace=None, decays=None):
        """An analyzer configured from a ChiSurf/CMC channel definition.

        `setup` is the shared detector-setup dict: ``detectors`` (name ->
        ``chs``, optional ``micro_time_ranges``, ``g_factor``/``l1``/``l2``),
        ``windows`` (name -> [lo, hi] micro-time channels), ``tttr_reading``
        (``macro_time_resolution`` in ns and ``micro_time_resolution`` in ps, as
        the detector-setup editors write them -- ``micro_time_resolution_unit``
        ``"ns"`` overrides the latter; used when the arguments are None), and
        setup-level ``g_factor``/``l1``/``l2``.

        `preset` chooses the streams, quantities and histograms:

        * ``single_color`` -- every detector; histograms of photons and duration.
        * ``two_color`` -- donor and acceptor detectors without alternation:
          ``Proximity Ratio`` = acceptor / (donor + acceptor).
        * ``pie`` -- prompt/delay micro-time windows: ``Proximity Ratio``
          (DA / (DD + DA)) and ``Stoichiometry`` ((DD + DA) / (DD + DA + AA));
          with `factors` also ``FRET Efficiency`` / ``Stoichiometry
          (corrected)``; the E-S histogram.
        * ``alex`` -- as ``pie``, with the excitation separated by the
          macro-time phase of an alternation period: `alex_period` [s] and
          `alex_windows` ``{prompt: (lo, hi), delay: (lo, hi)}`` in seconds of
          the period (default: the first and second half).
        * ``mfd`` -- polarization-resolved detectors ``<colour>_par`` /
          ``<colour>_perp``: per colour ``r Experimental (<colour>)``
          (``r Scatter (<colour>)`` with background subtraction), the
          proximity ratio over the donor/acceptor colours, and mean micro times;
          in prompt streams when the setup has a `prompt` window.
        * ``three_color_pie`` -- `colors` (blue, green, red) as detectors and as
          excitation windows of the same names: proximity ratios
          ``Proximity Ratio (BG)``, ``(BR)``, ``(GR)`` and stoichiometries
          ``Stoichiometry (BG)``, ``(GR)``.

        `factors` is ``{"gamma", "beta", "alpha", "delta"}`` for the corrected
        two-colour E/S; `search_channels` restricts the burst search; `trace`
        is ``(bin_width_s, max_bins)``; `decays` is ``(n_bins, micro_range,
        per_burst)``. `histograms=False` adds none.
        """
        if preset not in cls.PRESETS:
            raise ValueError(f"unknown preset {preset!r}; one of {cls.PRESETS}")
        # the reading routine, in the units the setup editors write (ChannelSetup.py)
        if macro_time_resolution is None:
            macro_time_resolution = setup_macro_time_resolution(setup) or 0.0
        if micro_time_resolution is None:
            micro_time_resolution = setup_micro_time_resolution(setup) or 0.0
        a = cls(float(macro_time_resolution), float(micro_time_resolution))
        detectors = dict(setup.get("detectors") or {})
        for name, d in detectors.items():
            ranges = [int(x) for r in (d.get("micro_time_ranges") or []) for x in r]
            a.add_detector(str(name), [int(c) for c in d.get("chs", [])], ranges)
        windows = dict(setup.get("windows") or {})
        if preset == "alex":
            if not alex_period:
                raise ValueError("preset 'alex' needs alex_period (seconds)")
            ticks = lambda s: int(round(s / macro_time_resolution))
            period = ticks(alex_period)
            aw = alex_windows or {prompt: (0.0, alex_period / 2), delay: (alex_period / 2, alex_period)}
            for name, (lo, hi) in aw.items():
                a.add_phase_window(str(name), period, ticks(lo), ticks(hi))
        else:
            for name, (lo, hi) in windows.items():
                a.add_window(str(name), int(lo), int(hi))
        a.set_search(int(min_photons), int(window_photons), float(window_time))
        if search_channels is not None:
            a.set_search_channels([int(c) for c in search_channels])
        bg = bool(subtract_background)
        hists = []

        if preset == "single_color":
            hists += [("Number of Photons", 50, 0, 500), ("Duration (ms)", 50, 0.0, 10.0)]
        elif preset == "two_color":
            a.add_ratio("Proximity Ratio", [acceptor], [donor, acceptor], bg)
            hists += [("Proximity Ratio", 50, -0.1, 1.1)]
        elif preset in ("pie", "alex"):
            dd, da, aa = f"{prompt} {donor}", f"{prompt} {acceptor}", f"{delay} {acceptor}"
            a.add_ratio("Proximity Ratio", [da], [dd, da], bg)
            a.add_ratio("Stoichiometry", [dd, da], [dd, da, aa], bg)
            hists += [("Proximity Ratio", 50, -0.1, 1.1, "Stoichiometry", 50, -0.1, 1.1)]
            if factors is not None:
                f = {"gamma": 1.0, "beta": 1.0, "alpha": 0.0, "delta": 0.0}
                f.update(factors)
                a.add_corrected_fret("FRET Efficiency", "Stoichiometry (corrected)", dd, da, aa,
                                     f["gamma"], f["beta"], f["alpha"], f["delta"], bg)
                hists += [("FRET Efficiency", 50, -0.1, 1.1, "Stoichiometry (corrected)", 50, -0.1, 1.1)]
        elif preset == "mfd":
            pre = (prompt + " ") if prompt in windows else ""
            colours = [n[:-4] for n in detectors if n.endswith("_par") and n[:-4] + "_perp" in detectors]
            if not colours:
                raise ValueError("preset 'mfd' needs <colour>_par / <colour>_perp detectors")
            def pol(c, key, default):
                d = detectors.get(c + "_par") or {}
                return float(d.get(key, setup.get(key, default)))
            for c in colours:
                par, perp = pre + c + "_par", pre + c + "_perp"
                g, l1, l2 = pol(c, "g_factor", 1.0), pol(c, "l1", 0.0), pol(c, "l2", 0.0)
                a.add_anisotropy(f"r Experimental ({c})", par, perp, g, l1, l2, False)
                if bg:
                    a.add_anisotropy(f"r Scatter ({c})", par, perp, g, l1, l2, True)
                if pre:   # without windows the table already has the detector's mean micro time
                    a.add_mean_microtime(f"Mean Microtime ({pre.strip()} {c}_par) (ns)", par)
                hists += [(f"r Experimental ({c})", 50, -0.2, 0.6)]
            if donor in colours and acceptor in colours:
                don = [pre + donor + "_par", pre + donor + "_perp"]
                acc = [pre + acceptor + "_par", pre + acceptor + "_perp"]
                a.add_ratio("Proximity Ratio", acc, don + acc, bg)
                hists += [("Proximity Ratio", 50, -0.1, 1.1, f"r Experimental ({donor})", 40, -0.2, 0.6)]
        elif preset == "three_color_pie":
            b, g, r = colors
            s = lambda w, d: f"{w} {d}"
            a.add_ratio("Proximity Ratio (BG)", [s(b, g)], [s(b, b), s(b, g)], bg)
            a.add_ratio("Proximity Ratio (BR)", [s(b, r)], [s(b, b), s(b, r)], bg)
            a.add_ratio("Proximity Ratio (GR)", [s(g, r)], [s(g, g), s(g, r)], bg)
            a.add_ratio("Stoichiometry (BG)", [s(b, b), s(b, g), s(b, r)],
                        [s(b, b), s(b, g), s(b, r), s(g, g), s(g, r)], bg)
            a.add_ratio("Stoichiometry (GR)", [s(g, g), s(g, r)], [s(g, g), s(g, r), s(r, r)], bg)
            hists += [("Proximity Ratio (GR)", 50, -0.1, 1.1, "Stoichiometry (GR)", 50, -0.1, 1.1),
                      ("Proximity Ratio (BG)", 50, -0.1, 1.1, "Proximity Ratio (BR)", 50, -0.1, 1.1)]
        if histograms:
            for h in hists:
                if len(h) == 4:
                    a.add_histogram(h[0], int(h[1]), float(h[2]), float(h[3]), "", 0, 0.0, 0.0)
                else:
                    a.add_histogram(h[0], int(h[1]), float(h[2]), float(h[3]),
                                    h[4], int(h[5]), float(h[6]), float(h[7]))
        if trace is not None:
            a.set_trace(float(trace[0]), int(trace[1]))
        if decays is not None:
            a.set_decays(int(decays[0]), int(decays[1]), bool(decays[2]) if len(decays) > 2 else False)
        return a

    def histograms(self):
        """{name: {"counts", "x", "x_edges"[, "y", "y_edges"]}}; the name is
        ``x`` or ``"x vs y"``, counts are (nx,) or (nx, ny)."""
        import numpy as np
        out = {}
        for i in range(self.histogram_count()):
            x, y = self.histogram_x(i), self.histogram_y(i)
            counts = np.asarray(self.histogram(i), dtype=float)
            h = {"x": x, "x_edges": np.asarray(self.histogram_x_edges(i), dtype=float)}
            if y:
                h["y"] = y
                h["y_edges"] = np.asarray(self.histogram_y_edges(i), dtype=float)
                counts = counts.reshape(self.histogram_nx(i), self.histogram_ny(i))
            h["counts"] = counts
            out[f"{x} vs {y}" if y else x] = h
        return out

    def snapshot(self):
        """Everything a live view draws (no photon is rescanned): counters,
        per-stream background (cps), histograms, and traces/decays when set."""
        import numpy as np
        streams = list(self.stream_names())
        snap = {
            "revision": int(self.revision()),
            "bursts": int(self.burst_count()),
            "photons": int(self.photon_count()),
            "elapsed_s": float(self.elapsed_seconds()),
            "burst_rate_hz": float(self.burst_rate_hz()),
            "background_cps": {s: float(self.background_rate(s)) for s in streams},
            "histograms": self.histograms(),
        }
        if self.trace_bin_width() > 0:
            snap["trace"] = {
                "bin_width_s": float(self.trace_bin_width()),
                "first_bin": int(self.trace_first_bin()),
                "counts": {s: np.asarray(self.trace(s), dtype=float) for s in streams},
            }
        if self.decay_bins() > 0:
            snap["decays"] = {s: np.asarray(self.decay(s, False), dtype=float) for s in streams}
            snap["burst_decays"] = {s: np.asarray(self.decay(s, True), dtype=float) for s in streams}
        return snap

    _INT_COLUMNS = ("First Photon", "Last Photon", "Number of Photons")

    def table(self):
        """The burst table as {column: numpy array}, in the batch table's names
        and order; photon indices and counts are int64."""
        import numpy as np
        out = {}
        for name in self.table_names():
            col = np.asarray(self.table_column(name), dtype=float)
            if name.startswith(self._INT_COLUMNS) or "(photons)" in name:
                col = col.astype(np.int64)
            out[name] = col
        return out

    def __repr__(self):
        return (f"StreamingBurstAnalyzer(L={self.min_photons()}, m={self.window_photons()}, "
                f"T={self.window_time():.3g} s, bursts={self.burst_count()}, "
                f"photons={self.photon_count()})")
    %}
#endif
}
