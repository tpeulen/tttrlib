# SPDX-License-Identifier: BSD-3-Clause
"""A/B of rcm_from_dye_solutions against the chisurf transcription."""
import numpy as np
import pytest

import tttrlib
from afret_reference import rcm as ref

LAYOUTS = [
    [("A", "P"), ("D", "P")],
    [("D", "P"), ("A", "P"), ("D", "S")][:2],
    [("A", "P"), ("D", "P"), ("A", "S"), ("D", "S")],
    [("D", "S"), ("A", "P"), ("A", "S"), ("D", "P")],
    [("A", "P"), ("D", "P"), ("A", "P"), ("D", "P")],
    [("A", "P"), ("X", "P"), ("D", "P")],
]


@pytest.mark.parametrize("layout", LAYOUTS)
@pytest.mark.parametrize("seed", [0, 1])
def test_rcm_matches_reference(layout, seed):
    rng = np.random.default_rng(seed)
    n = len(layout)
    donor = rng.uniform(0.01, 0.2, n) + np.array([1.0 if s == "D" else 0 for s, _ in layout])
    acceptor = rng.uniform(0.01, 0.2, n) + np.array([1.0 if s == "A" else 0 for s, _ in layout])
    kw = dict(absorbance_ratio=0.8, detector_assignment=layout, anisotropy=(0.05, 0.12))
    got = tttrlib.rcm_from_dye_solutions(donor, acceptor, **kw)
    want = ref.rcm_from_dye_solutions(donor, acceptor, **kw)
    np.testing.assert_allclose(got, want, rtol=1e-12, atol=1e-13)


def test_rcm_rejects_bad_layouts():
    with pytest.raises(ValueError):
        tttrlib.rcm_from_dye_solutions([1, 2], [1, 2], 1.0, [("A", "P")])
    with pytest.raises(ValueError):
        tttrlib.rcm_from_dye_solutions([1, 2], [1, 2], 1.0, [("A", "P"), ("A", "P")])
