
# --- channel setup: reading a detector setup, its range dialects and units ---------------------------
#
# A detector setup (the channel definition ChiSurf's and CMC's setup editors write, and that
# StreamingBurstAnalyzer.from_setup reads) is a dict:
#
#     {"detectors": {name: {"chs": [routing channels], "micro_time_ranges": [[start, stop), ...],
#                           "ch_p": [...], "ch_s": [...]}},
#      "windows": {name: [start, stop)},
#      "tttr_reading": {"macro_time_resolution": ns, "micro_time_resolution": ps,
#                       "micro_time_resolution_unit": "ps" | "ns" | "s"}}
#
# optionally wrapped as {"data": {...}}. Windows and gates are **half-open** micro-time channel
# ranges of the setup's micro-time resolution. These functions are the one reader every consumer
# (tttrlib, ChiSurf, ndXplorer) uses, so the same setup means the same photons everywhere.

import numpy as _cs_np

_SETUP_TIME_UNITS = {"ps": 1e-12, "ns": 1e-9, "s": 1.0}


def setup_definition(setup):
    """The definition itself: *setup*, or its ``"data"`` when wrapped (``{}`` for a non-dict)."""
    if not isinstance(setup, dict):
        return {}
    inner = setup.get("data")
    return dict(inner) if isinstance(inner, dict) else dict(setup)


def setup_detectors(setup):
    """``{detector: [routing channels]}`` of the detectors that name channels."""
    det = setup_definition(setup).get("detectors") or {}
    return {
        str(k): [int(c) for c in (v.get("chs") or [])]
        for k, v in det.items()
        if isinstance(v, dict) and v.get("chs")
    }


def setup_gates(setup):
    """``{detector: [[start, stop), ...]}`` -- the micro-time gates of the gated detectors."""
    det = setup_definition(setup).get("detectors") or {}
    return {
        str(k): [[int(r[0]), int(r[1])] for r in (v.get("micro_time_ranges") or [])]
        for k, v in det.items()
        if isinstance(v, dict) and v.get("micro_time_ranges")
    }


def setup_windows(setup):
    """``{window: [start, stop)}`` -- the setup's excitation windows (a ``None`` window is left out)."""
    win = setup_definition(setup).get("windows") or {}
    return {
        str(k): [int(v[0]), int(v[1])]
        for k, v in win.items()
        if isinstance(v, (list, tuple)) and len(v) >= 2 and None not in v[:2]
    }


def setup_micro_time_resolution(setup):
    """The micro-time channel width of the setup's windows and gates, in seconds (``None`` when the
    setup does not say). ``tttr_reading.micro_time_resolution`` is in ps, as the setup editors
    write it; ``micro_time_resolution_unit`` (``ps`` / ``ns`` / ``s``) says otherwise."""
    reading = setup_definition(setup).get("tttr_reading") or {}
    unit = str(reading.get("micro_time_resolution_unit", "ps")).lower()
    if unit not in _SETUP_TIME_UNITS:
        raise ValueError(f"micro_time_resolution_unit {unit!r}: one of ps, ns, s")
    try:
        value = float(reading.get("micro_time_resolution") or 0.0)
    except (TypeError, ValueError):
        return None
    return value * _SETUP_TIME_UNITS[unit] if value > 0 else None


def setup_macro_time_resolution(setup):
    """The setup's macro-time tick in seconds (``tttr_reading.macro_time_resolution``, ns);
    ``None`` when it does not say."""
    reading = setup_definition(setup).get("tttr_reading") or {}
    try:
        value = float(reading.get("macro_time_resolution") or 0.0)
    except (TypeError, ValueError):
        return None
    return value * 1e-9 if value > 0 else None


def rescale_micro_time_ranges(ranges, from_resolution, to_resolution):
    """Ranges given in channels of *from_resolution* (s) in channels of *to_resolution*: the same
    time span, bounds rounded to the nearest channel. A dict of ranges (windows) or of range lists
    (gates) keeps its shape."""
    f = (float(from_resolution) / float(to_resolution)
         if from_resolution and to_resolution else 1.0)

    def one(r):
        return [int(round(r[0] * f)), int(round(r[1] * f))]

    if isinstance(ranges, dict):
        return {
            str(k): ([one(r) for r in v] if not v or isinstance(v[0], (list, tuple)) else one(v))
            for k, v in ranges.items()
        }
    return [one(r) for r in (ranges or [])]


def ranges_to_inclusive(ranges):
    """Half-open ``[start, stop)`` ranges as inclusive ``(start, stop - 1)`` (what BVA, TwoCDE,
    BurstFeature and CLSMImage.fill take)."""
    return [(int(r[0]), max(int(r[0]), int(r[1]) - 1)) for r in (ranges or []) if len(r) >= 2]


def ranges_from_inclusive(ranges):
    """Inclusive ``(start, stop)`` ranges as half-open ``[start, stop + 1)``."""
    return [[int(r[0]), int(r[1]) + 1] for r in (ranges or []) if len(r) >= 2]


def micro_time_mask(micro_times, ranges, inclusive=False):
    """Photons whose micro time is in any of *ranges* (half-open, or inclusive with *inclusive*);
    no ranges: every photon. A boolean array like *micro_times*."""
    micro = _cs_np.asarray(micro_times)
    if not ranges:
        return _cs_np.ones(micro.shape, dtype=bool)
    keep = _cs_np.zeros(micro.shape, dtype=bool)
    for r in ranges:
        lo, hi = int(r[0]), int(r[1])
        keep |= (micro >= lo) & ((micro <= hi) if inclusive else (micro < hi))
    return keep


def setup_channel_map(setup, ungated_means_all=False, per_detector_without_windows=False):
    """The window x detector stream map: ``{"<window>_<detector>": [{"window_range", "detector_chs",
    "micro_time_range"}, ...]}``, one entry per gate. *ungated_means_all*: a detector without gates
    gets one entry with gate ``None`` (all photons). *per_detector_without_windows*: a setup without
    windows gives one stream per detector (``"<detector>"``, window ``None``)."""
    s = setup_definition(setup)
    windows = dict(s.get("windows") or {})
    if not windows and per_detector_without_windows:
        windows = {"": None}
    detectors = {str(k): v for k, v in (s.get("detectors") or {}).items() if isinstance(v, dict)}
    out = {}
    for wname, wrange in windows.items():
        for dname, info in detectors.items():
            gates = list(info.get("micro_time_ranges") or [])
            if not gates and ungated_means_all:
                gates = [None]
            out[f"{wname}_{dname}" if wname else dname] = [
                {"window_range": wrange, "detector_chs": list(info.get("chs") or []),
                 "micro_time_range": g}
                for g in gates
            ]
    return out


def polarization_channels(detector, single_is_both=True):
    """A detector's ``(parallel, perpendicular)`` routing channels: explicit ``ch_p`` / ``ch_s``,
    else ``chs`` split even (parallel) / odd (perpendicular). A single channel is both with
    *single_is_both* (intensity images), else parallel only (VV/VH histograms, MLE)."""
    ch_p = [int(x) for x in (detector.get("ch_p") or [])]
    ch_s = [int(x) for x in (detector.get("ch_s") or [])]
    if ch_p or ch_s:
        return ch_p, ch_s
    chs = [int(x) for x in (detector.get("chs") or [])]
    if not chs:
        return [], []
    return chs[::2], (chs[1::2] if len(chs) > 1 else (chs if single_is_both else []))


# --- micro-time windows: windows and gates together with the channel width they are in -------------
#
# Windows and gates are channel numbers, and a channel number means nothing without its width: the
# same "prompt 0-2048" is the first half of a 50 ps x 4096 SPC-130 range and the first 65 ns of a
# 32 ps HydraHarp range. So wherever windows travel between a setup, a GUI, settings, a stored
# analysis and an analysis call, they travel as one record:
#
#     {"windows": {name: [start, stop)},
#      "gates": {detector: [[start, stop), ...]},
#      "micro_time_resolution_ps": 32.0}        # None: the width is not known ("raw" channels)
#
# A record without a width (``None``) is honest about not knowing it -- an analysis stored before
# the width was recorded -- and is applied as raw channels; whoever applies it says so.


def micro_time_windows(windows=None, gates=None, micro_time_resolution_ps=None):
    """The record windows and gates travel in (see above): half-open integer ranges, a ``None``
    window left out, a non-positive width read as unknown (``None``)."""
    win = {}
    for k, v in dict(windows or {}).items():
        if isinstance(v, (list, tuple)) and len(v) >= 2 and None not in v[:2]:
            win[str(k)] = [int(v[0]), int(v[1])]
        elif v is not None:
            raise TypeError(f"window {k!r}: [start, stop) expected, got {v!r}")
    gat = {}
    for k, rs in dict(gates or {}).items():
        ranges = [[int(r[0]), int(r[1])] for r in (rs or [])]
        if ranges:
            gat[str(k)] = ranges
    width = float(micro_time_resolution_ps) if micro_time_resolution_ps else 0.0
    return {"windows": win, "gates": gat,
            "micro_time_resolution_ps": width if width > 0 else None}


def is_micro_time_windows(value):
    """Whether *value* is a windows record (has ``windows`` and ``micro_time_resolution_ps``)."""
    return (isinstance(value, dict) and "windows" in value
            and "micro_time_resolution_ps" in value)


def as_micro_time_windows(value):
    """*value* as a windows record; raises ``TypeError`` for bare ``{name: [start, stop]}``
    windows, which do not say which channel width they are in. ``None`` is the empty record."""
    if value is None:
        return micro_time_windows()
    if not is_micro_time_windows(value):
        raise TypeError(
            "micro-time windows travel with their channel width: "
            "{'windows': ..., 'gates': ..., 'micro_time_resolution_ps': ...} "
            "(tttrlib.micro_time_windows / setup_micro_time_windows), got " + repr(value)[:120])
    return micro_time_windows(value.get("windows"), value.get("gates"),
                              value.get("micro_time_resolution_ps"))


def setup_micro_time_windows(setup):
    """A setup's windows and detector gates with the channel width they are in (the record)."""
    width = setup_micro_time_resolution(setup)
    return micro_time_windows(setup_windows(setup), setup_gates(setup),
                              width * 1e12 if width else None)


def micro_time_windows_at(record, micro_time_resolution):
    """*record* in channels of *micro_time_resolution* (s): the same time spans, bounds rounded to
    the nearest channel, and the record says the new width. A record of unknown width is returned
    unchanged (its channels cannot be converted; it stays ``None``)."""
    rec = as_micro_time_windows(record)
    src = rec["micro_time_resolution_ps"]
    if not src or not micro_time_resolution or micro_time_resolution <= 0:
        return rec
    dst_ps = float(micro_time_resolution) * 1e12
    if abs(src / dst_ps - 1.0) <= 1e-9:
        return rec
    return micro_time_windows(
        rescale_micro_time_ranges(rec["windows"], src * 1e-12, micro_time_resolution),
        rescale_micro_time_ranges(rec["gates"], src * 1e-12, micro_time_resolution),
        dst_ps,
    )


def micro_time_windows_setup(record, setup=None):
    """*setup* (or an empty one) with the record's windows, its gates on the setup's detectors
    (a detector the record gives no gate is ungated) and ``tttr_reading.micro_time_resolution``
    set to the record's width (ps; left out when the width is unknown)."""
    rec = as_micro_time_windows(record)
    s = setup_definition(setup)
    s["windows"] = {k: list(v) for k, v in rec["windows"].items()}
    detectors = {}
    for name, info in (s.get("detectors") or {}).items():
        d = dict(info) if isinstance(info, dict) else {}
        gates = rec["gates"].get(str(name))
        if gates:
            d["micro_time_ranges"] = [list(r) for r in gates]
        else:
            d.pop("micro_time_ranges", None)
        detectors[name] = d
    s["detectors"] = detectors
    reading = dict(s.get("tttr_reading") or {})
    reading.pop("micro_time_resolution_unit", None)
    if rec["micro_time_resolution_ps"]:
        reading["micro_time_resolution"] = rec["micro_time_resolution_ps"]
    else:
        reading.pop("micro_time_resolution", None)
    s["tttr_reading"] = reading
    return s
