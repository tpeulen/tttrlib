# SPDX-License-Identifier: BSD-3-Clause
"""A/B of the AccurateFret automatic calibration against the chisurf reference.

Reference: `afret_reference/calibrate.py` and `auto.py`, the numpy
transcription of chisurf's `auto_calibrate`, `global_es_correction` and the
gamma/beta estimators. Synthetic ALEX bursts with known factors, and the cal1
ALEX/PIE measurement when the test data are present.
"""
import os
import subprocess
import sys

import numpy as np
import pytest

import tttrlib
from afret_reference import auto as ref_auto
from afret_reference import calibrate as ref
from afret_reference import live
from test_accurate_fret_populations import _alex_bursts

FACT = dict(rtol=1e-10, atol=1e-12)
CAL1 = os.path.join(os.environ.get("TTTRLIB_DATA", "/Users/tpeulen/dev/tttr-data"),
                    "sm", "cal1", "001_60g_25r_cal1_cy3b_8_18_33bp_atto647n_alex.pto")


def _line(tau_d0=4.0):
    tau = np.linspace(0.05, tau_d0, 60)
    # a gently curved line, so interpolation between knots is exercised
    return tau, np.clip(1.0 - tau / tau_d0 + 0.03 * np.sin(np.pi * tau / tau_d0), 0, 1)


def _lifetimes(dd, da, tau_d0=4.0, seed=0):
    rng = np.random.default_rng(seed)
    e = da / np.maximum(dd + da, 1)
    return tau_d0 * (1 - e) + rng.normal(0, 0.1, dd.size)


def test_global_es_correction():
    dd, da, aa = _alex_bursts(0)
    labels = np.repeat([0, 1, 2, 1], [150, 120, 250, 300])
    for kw in (dict(), dict(alpha=0.07, delta=0.05)):
        got, want = tttrlib.global_es_correction(dd, da, aa, labels, **kw), \
            ref.global_es_correction(dd, da, aa, labels, **kw)
        for key in want:
            np.testing.assert_allclose(got[key], want[key], rtol=1e-12, err_msg=key)
    with pytest.raises(ValueError):
        tttrlib.global_es_correction(dd, da, aa, np.zeros(dd.size, int))


def test_beta_from_stoichiometry():
    dd, da, aa = _alex_bursts(1)
    kw = dict(gamma=0.8, alpha=0.07, delta=0.05, bg_dd=0.5, bg_da=0.4, bg_aa=0.3)
    assert tttrlib.beta_from_stoichiometry(dd, da, aa, **kw) == pytest.approx(
        ref.beta_from_stoichiometry(dd, da, aa, **kw), rel=1e-12)
    assert tttrlib.beta_from_stoichiometry(dd, da, -aa, gamma=1.0) == 1.0


@pytest.mark.parametrize("delta", [0.0, 0.05])
def test_gamma_from_lifetime(delta):
    dd, da, aa = _alex_bursts(2)
    tau = _lifetimes(dd, da)
    tau[5] = np.nan
    labels = np.repeat([-1, 0, 1, 2], [150, 120, 250, 300])
    lt, le = _line()
    kw = dict(i_aa=aa, alpha=0.07, delta=delta, bg_dd=0.5, labels=labels)
    got = tttrlib.gamma_from_lifetime(dd, da, tau, line=(lt, le), **kw)
    want = ref.gamma_from_lifetime(dd, da, tau, line=ref.Line(lt, le), **kw)
    np.testing.assert_allclose([got["gamma"], got["sigma"]], [want["gamma"], want["sigma"]],
                               rtol=1e-12)
    assert [p["label"] for p in got["populations"]] == [p["label"] for p in want["populations"]]


def test_lightpath_correction_factors():
    got = tttrlib.lightpath_correction_factors(0.9, 0.06, 0.8, 0.07, 0.9, gG=1.1, gR=0.8,
                                               qy_d=0.6, qy_a=0.4)
    assert got["gamma"] == pytest.approx((0.8 * 0.8 * 0.4) / (1.1 * 0.9 * 0.6), rel=1e-15)
    assert got["alpha"] == pytest.approx((0.8 * 0.06) / (1.1 * 0.9), rel=1e-15)
    assert got["delta"] == pytest.approx(0.07 / 0.9, rel=1e-15)
    assert np.isnan(tttrlib.lightpath_correction_factors(0, 0, 1, 0, 0)["gamma"])


def _compare(got, want):
    for key in ("gamma", "alpha", "beta", "delta"):
        np.testing.assert_allclose(got["factors"][key], want["factors"][key], **FACT, err_msg=key)
    for key in ("donor_only", "acceptor_only", "fret", "fret_labels"):
        np.testing.assert_array_equal(got["split"][key], want["split"][key], err_msg=key)
    for key, value in want["gamma_estimates"].items():
        np.testing.assert_allclose(got["gamma_estimates"][key], value, **FACT, err_msg=key)
    for key in ("gamma", "alpha", "delta"):
        np.testing.assert_allclose(got["uncertainties"][key], want["uncertainties"][key], **FACT)
    assert got["estimated"] == want["estimated"]
    assert (got["iterations"], got["converged"]) == (want["iterations"], want["converged"])
    assert got["messages"] == want["messages"]


CASES = [
    dict(),
    dict(priors={"gamma": (0.9, 0.1), "alpha": (0.06, 0.02), "delta": (0.04, 0.02)}),
    dict(gamma_source="lifetime", lifetimes=True),
    dict(gamma_source="combined", lifetimes=True, priors={"gamma": (0.7, 0.05)}),
    dict(fret=((0.45, 400),)),
    dict(no_alex=True),
    dict(n_iterations=1),
    dict(start=dict(gamma=50.0, alpha=-1.0, bg_dd=1.0, bg_da=0.5, bg_aa=0.2)),
]


@pytest.mark.parametrize("case", range(len(CASES)))
@pytest.mark.parametrize("seed", [0, 1])
def test_auto_calibrate(case, seed):
    c = dict(CASES[case])
    burst_kw = {"fret": c.pop("fret")} if "fret" in c else {}
    dd, da, aa = _alex_bursts(seed, **burst_kw)
    if c.pop("no_alex", False):
        aa = None
    tau = _lifetimes(dd, da, seed=seed) if c.pop("lifetimes", False) else None
    priors = c.pop("priors", {})
    start = c.pop("start", {})
    lt, le = _line()
    columns = {"i_dd": dd, "i_da": da, "i_aa": aa, "tau_f": tau}
    got = tttrlib.auto_calibrate(columns, dict(start, priors=priors), dict(c, line=(lt, le)))
    want = ref_auto.auto_calibrate(dd, da, aa, calib=ref.Calib(priors=priors, **start), tau_f=tau,
                                   line=ref.Line(lt, le), **c)
    _compare(got, want)


def test_auto_calibrate_recovers_the_true_factors():
    dd, da, aa = _alex_bursts(3, n_do=600, n_ao=500, fret=((0.25, 1500), (0.7, 1500)))
    got = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa})
    f = got["factors"]
    assert f["gamma"] == pytest.approx(0.8, rel=0.1)
    assert f["alpha"] == pytest.approx(0.07, abs=0.01)
    assert f["delta"] == pytest.approx(0.05, abs=0.01)
    assert got["converged"]


def test_auto_calibrate_progress_can_cancel():
    dd, da, aa = _alex_bursts(0)
    seen = []
    got = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa},
                                 progress=lambda i, n, m: seen.append((i, n)) or False)
    assert seen == [(1, 6)] and got["cancelled"] and not got["converged"]
    assert got["iterations"] == 1
    assert "stopped by the caller after 1 iteration(s)" in got["messages"]


_CAL1_READ = """
import sys, numpy as np
from ndxplorer.io.loading import read
t = read(sys.argv[1])
col = lambda name: np.asarray(t.column_values(name), dtype=float)
# green/red under the prompt (donor) pulse, yellow = red detector under the delayed pulse
np.savez(sys.argv[2], dd=col("Number of Photons (green)"), da=col("Number of Photons (red)"),
         aa=col("Number of Photons (yellow)"))
"""


@pytest.fixture(scope="module")
def cal1_columns(tmp_path_factory):
    """Burst columns of the cal1 measurement, read by ndxplorer.

    Read in a child interpreter against the *installed* tttrlib: the reader
    needs the whole PTO stack, which a partial development build (e.g. the
    split extensions) may not wire up, and the columns do not depend on the
    code under test.
    """
    if not os.path.exists(CAL1):
        pytest.skip("cal1 test data not present")
    out = tmp_path_factory.mktemp("cal1") / "columns.npz"
    env = {k: v for k, v in os.environ.items() if k != "PYTHONPATH"}
    proc = subprocess.run([sys.executable, "-c", _CAL1_READ, CAL1, str(out)], env=env,
                          capture_output=True, text=True, cwd=str(out.parent))
    if proc.returncode != 0:
        pytest.skip("ndxplorer could not read cal1: " + proc.stderr.strip().splitlines()[-1])
    data = np.load(out)
    return data["dd"], data["da"], data["aa"]


def test_auto_calibrate_cal1_measurement(cal1_columns):
    dd, da, aa = cal1_columns
    got = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa})
    want = ref_auto.auto_calibrate(dd, da, aa)
    _compare(got, want)
    counts = got["split"]["counts"]
    assert counts["donor_only"] > 0 and counts["acceptor_only"] > 0 and counts["fret"] > 0


def test_transcription_matches_chisurf():
    acc = live.load("core.fluorescence.fret.accurate")
    cal = live.load("core.fluorescence.fret.calibration")
    if acc is None or cal is None:
        pytest.skip("chisurf's accurate.py is gone; the transcription stands on its own")
    dd, da, aa = _alex_bursts(1)
    labels = np.repeat([0, 1, 2, 1], [150, 120, 250, 300])
    a = cal.global_es_correction(dd, da, aa, labels, alpha=0.05)
    b = ref.global_es_correction(dd, da, aa, labels, alpha=0.05)
    assert a == b
    calib = cal.CalibrationParameters()
    calib._gamma.prior = cal.NormalPrior(mu=0.9, sigma=0.1)
    a = acc.auto_calibrate(dd, da, aa, calibration=calib)
    b = ref_auto.auto_calibrate(dd, da, aa, calib=ref.Calib(priors={"gamma": (0.9, 0.1)}))
    for key in ("gamma", "alpha", "beta", "delta"):
        assert a.factors[key] == pytest.approx(b["factors"][key], rel=1e-12)
    assert a.messages == b["messages"] and a.iterations == b["iterations"]
    np.testing.assert_array_equal(a.split.fret_labels, b["split"]["fret_labels"])
