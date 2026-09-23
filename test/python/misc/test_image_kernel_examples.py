# SPDX-License-Identifier: BSD-3-Clause
"""Smoke test for the image-kernel gallery example.

Runs ``examples/miscellaneous/plot_image_kernels.py`` end to end (headless)
and checks the numbers the example itself reports, so a change to any of the
wrapped kernels cannot break the tutorial silently.
"""

import runpy
from pathlib import Path

import numpy as np
import pytest

try:
    import matplotlib
    matplotlib.use("Agg")
except ImportError:  # pragma: no cover
    pytest.skip("the gallery examples need matplotlib", allow_module_level=True)

_EXAMPLES = Path(__file__).resolve().parents[3] / "examples" / "miscellaneous"


@pytest.mark.slow
def test_image_kernels_example():
    ns = runpy.run_path(str(_EXAMPLES / "plot_image_kernels.py"), run_name="__main__")
    # the example prints these; re-derive the claims from its namespace
    counts = ns["counts"]
    clean = ns["clean"]
    hot = ns["hot"]
    # the median removed the impulse from (nearly) every hot pixel
    assert (counts[hot] > clean[hot]).sum() >= int(0.9 * hot.sum())
    # moment centroid within 0.5 px of the true spot centre
    assert abs(ns["cx"] - 41.3) < 0.5
    assert abs(ns["cy"] - 55.7) < 0.5
    # drift recovered within 0.3 px of the integer shift (3, -1)
    assert abs(ns["dx"] - 3.0) < 0.3
    assert abs(ns["dy"] + 1.0) < 0.3
    # the rectangle sum equals the direct window sum
    assert ns["s"] == pytest.approx(clean[50:63, 36:49].sum(), abs=1)
