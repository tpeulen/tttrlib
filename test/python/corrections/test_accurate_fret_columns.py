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


#: The same table under the detector names emission|excitation.
NAMED = ["Number of Photons (G|G)", "Number of Photons (R|G)", "Number of Photons (R|R)",
         "S prompt G|G (photons) | 616-3784", "S prompt R|G (photons) | 616-3784",
         "S prompt R|R (photons) | 616-3784", "S delayed G|G (photons) | 4278-7762",
         "S delayed R|G (photons) | 4278-7762", "S delayed R|R (photons) | 4278-7762",
         "G|G Count Rate (KHz)", "Tau (G|G)"]


def test_detector_names_map_onto_the_roles():
    got = tttrlib.guess_burst_columns(NAMED)
    assert got == {"i_dd": "S prompt G|G (photons) | 616-3784",
                   "i_da": "S prompt R|G (photons) | 616-3784",
                   "i_aa": "S delayed R|R (photons) | 4278-7762",
                   "tau_f": "Tau (G|G)"}
    plain = ["G|G Count Rate (KHz)", "R|G Count Rate (KHz)", "R|R Count Rate (KHz)", "Tau (G|G)"]
    assert tttrlib.guess_burst_columns(plain) == {
        "i_dd": "G|G Count Rate (KHz)", "i_da": "R|G Count Rate (KHz)",
        "i_aa": "R|R Count Rate (KHz)", "tau_f": "Tau (G|G)"}


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


def test_a_range_written_lo_colon_hi_is_a_gated_stream():
    """Ranges are written ``lo:hi``; a table written before says ``lo-hi`` and reads the same."""
    import tttrlib

    for sep in (":", "-"):
        names = ["Number of Photons (G|G)", "Number of Photons (R|G)", "Number of Photons (R|R)",
                 f"S prompt G|G (photons) | 616{sep}3784", f"S prompt R|G (photons) | 616{sep}3784",
                 f"S delayed R|R (photons) | 4278{sep}7762", "G|G Count Rate (KHz)"]
        got = tttrlib.guess_burst_columns(names)
        assert got["i_dd"] == f"S prompt G|G (photons) | 616{sep}3784", sep
        assert got["i_aa"] == f"S delayed R|R (photons) | 4278{sep}7762", sep
