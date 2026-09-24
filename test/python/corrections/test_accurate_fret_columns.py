# SPDX-License-Identifier: BSD-3-Clause
"""Burst-table channel conventions and the calibration report."""
import numpy as np

import tttrlib
from test_accurate_fret_populations import _alex_bursts

CAL1 = ["Number of Photons (green)", "Number of Photons (red)", "Number of Photons (yellow)",
        "S prompt green (photons) | 616-3784", "S prompt red (photons) | 616-3784",
        "S prompt yellow (photons) | 616-3784", "S delayed green (photons) | 4278-7762",
        "S delayed red (photons) | 4278-7762", "S delayed yellow (photons) | 4278-7762",
        "S prompt green (kHz) | 616-3784", "Green Count Rate (KHz)", "<tauD(A)>x"]


def test_gated_photon_columns_win_and_aa_uses_the_second_acceptor_detector():
    got = tttrlib.guess_burst_columns(CAL1)
    assert got == {"i_dd": "S prompt green (photons) | 616-3784",
                   "i_da": "S prompt red (photons) | 616-3784",
                   "i_aa": "S delayed yellow (photons) | 4278-7762",
                   "tau_f": "<tauD(A)>x"}


def test_plain_names_and_extra_hints():
    names = ["i_dd", "i_da", "i_aa", "det0_life"]
    assert tttrlib.guess_burst_columns(names, {"tau_f": ("det0_life",)})["tau_f"] == "det0_life"
    assert tttrlib.gated_stream_columns(names) == {}


def test_report_names_every_factor():
    dd, da, aa = _alex_bursts(0)
    r = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa})
    text = tttrlib.calibration_report(r, ["seeded by hand"])
    for key in ("alpha", "delta", "gamma", "beta", "converged", "! seeded by hand"):
        assert key in text
