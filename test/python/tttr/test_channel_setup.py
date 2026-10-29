"""Reading a detector setup: one reader for tttrlib's from_setup, ChiSurf and ndXplorer.

Windows and gates are half-open micro-time channels of the setup's resolution (ps, as the setup
editors write it); TTTRMask.select_microtime_ranges keeps exactly those channels.
"""

import numpy as np
import pytest

import tttrlib

SETUP = {
    "detectors": {"green": {"chs": [0, 8], "micro_time_ranges": [[0, 2048]]},
                  "red": {"chs": [1, 9]}, "broken": "not a dict"},
    "windows": {"prompt": [0, 2048], "delayed": [2048, 4096], "all": None},
    "tttr_reading": {"micro_time_resolution": 50.0, "macro_time_resolution": 12.5},
}


def test_detectors_gates_windows_and_units():
    assert tttrlib.setup_detectors(SETUP) == {"green": [0, 8], "red": [1, 9]}
    assert tttrlib.setup_gates(SETUP) == {"green": [[0, 2048]]}
    assert tttrlib.setup_windows(SETUP) == {"prompt": [0, 2048], "delayed": [2048, 4096]}
    assert tttrlib.setup_micro_time_resolution(SETUP) == pytest.approx(50e-12)
    assert tttrlib.setup_macro_time_resolution({"data": SETUP}) == pytest.approx(12.5e-9)
    ns = {"tttr_reading": {"micro_time_resolution": 0.05, "micro_time_resolution_unit": "ns"}}
    assert tttrlib.setup_micro_time_resolution(ns) == pytest.approx(50e-12)
    assert tttrlib.setup_micro_time_resolution({}) is None
    with pytest.raises(ValueError):
        tttrlib.setup_micro_time_resolution({"tttr_reading": {"micro_time_resolution_unit": "x"}})


def test_ranges_convert_between_resolutions_and_dialects():
    assert tttrlib.rescale_micro_time_ranges({"prompt": [0, 2048]}, 50e-12, 32e-12) == \
        {"prompt": [0, 3200]}
    assert tttrlib.rescale_micro_time_ranges({"g": [[0, 2048]]}, 50e-12, 32e-12) == {"g": [[0, 3200]]}
    assert tttrlib.ranges_to_inclusive([[0, 2048]]) == [(0, 2047)]
    assert tttrlib.ranges_from_inclusive([(0, 2047)]) == [[0, 2048]]
    micro = np.arange(10)
    assert np.flatnonzero(tttrlib.micro_time_mask(micro, [[2, 5]])).tolist() == [2, 3, 4]
    assert tttrlib.micro_time_mask(micro, [(2, 4)], inclusive=True).tolist() == \
        tttrlib.micro_time_mask(micro, [[2, 5]]).tolist()


def test_channel_map_and_polarization():
    m = tttrlib.setup_channel_map(SETUP)
    assert m["prompt_green"] == [{"window_range": [0, 2048], "detector_chs": [0, 8],
                                  "micro_time_range": [0, 2048]}]
    assert m["prompt_red"] == []
    assert tttrlib.setup_channel_map(SETUP, ungated_means_all=True)["prompt_red"][0][
        "micro_time_range"] is None
    per = tttrlib.setup_channel_map({"detectors": SETUP["detectors"]},
                                    per_detector_without_windows=True)
    assert set(per) == {"green", "red"}
    assert tttrlib.polarization_channels({"chs": [0, 8]}) == ([0], [8])
    assert tttrlib.polarization_channels({"chs": [3]}) == ([3], [3])
    assert tttrlib.polarization_channels({"chs": [3]}, single_is_both=False) == ([3], [])
    assert tttrlib.rescale_micro_time_ranges({"g": []}, 50e-12, 32e-12) == {"g": []}


def test_from_setup_reads_the_same_units():
    a = tttrlib.StreamingBurstAnalyzer.from_setup(
        {**SETUP, "windows": tttrlib.setup_windows(SETUP),
         "detectors": {"green": {"chs": [0]}, "red": {"chs": [1]}}}, preset="pie")
    assert a.micro_time_resolution() == pytest.approx(50e-12)
    assert a.macro_time_resolution() == pytest.approx(12.5e-9)


def test_mask_keeps_the_half_open_range():
    t = tttrlib.TTTR(np.arange(10, dtype=np.uint64), np.arange(10, dtype=np.uint16),
                     np.zeros(10, np.int8), np.zeros(10, np.int8))
    m = tttrlib.TTTRMask()
    m.select_microtime_ranges(t, [(2, 5)])
    assert list(m.get_indices(True)) == [2, 3, 4]


# --- the windows record: windows and gates with the channel width they are in ----------------------

def test_record_from_setup_carries_the_width():
    rec = tttrlib.setup_micro_time_windows(SETUP)
    assert rec == {"windows": {"prompt": [0, 2048], "delayed": [2048, 4096]},
                   "gates": {"green": [[0, 2048]]}, "micro_time_resolution_ps": pytest.approx(50.0)}
    assert tttrlib.is_micro_time_windows(rec)
    assert not tttrlib.is_micro_time_windows({"prompt": [0, 10]})


def test_record_converts_to_the_data_width_exactly():
    # SPC-130 50 ps x 4096 setup applied to a 32 ps HydraHarp stream: the same time spans
    rec = tttrlib.setup_micro_time_windows(SETUP)
    at = tttrlib.micro_time_windows_at(rec, 32e-12)
    assert at["micro_time_resolution_ps"] == pytest.approx(32.0)
    assert at["windows"] == {"prompt": [0, 3200], "delayed": [3200, 6400]}
    assert at["gates"] == {"green": [[0, 3200]]}
    # same width: unchanged; unknown width: cannot convert, stays raw
    assert tttrlib.micro_time_windows_at(rec, 50e-12) == rec
    raw = tttrlib.micro_time_windows({"prompt": [0, 10]})
    assert raw["micro_time_resolution_ps"] is None
    assert tttrlib.micro_time_windows_at(raw, 32e-12) == raw


def test_bare_windows_are_refused():
    with pytest.raises(TypeError):
        tttrlib.as_micro_time_windows({"prompt": [0, 2048]})
    with pytest.raises(TypeError):
        tttrlib.micro_time_windows({"prompt": 5})
    assert tttrlib.as_micro_time_windows(None) == tttrlib.micro_time_windows()


def test_record_back_into_a_setup():
    rec = tttrlib.micro_time_windows({"prompt": [0, 10]}, {"red": [[5, 9]]}, 16.0)
    s = tttrlib.micro_time_windows_setup(rec, SETUP)
    assert s["windows"] == {"prompt": [0, 10]}
    assert s["detectors"]["red"]["micro_time_ranges"] == [[5, 9]]
    assert "micro_time_ranges" not in s["detectors"]["green"]
    assert tttrlib.setup_micro_time_resolution(s) == pytest.approx(16e-12)
    assert tttrlib.setup_micro_time_windows(s) == rec
