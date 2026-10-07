"""Fit24 reports the effective mixture used for its fitted curve and score.

The recorded BH SPC132 counts use estimated IRF/background and provider-derived
timing. They reproduce a reporting defect, not an instrument calibration or a
claim that a two-lifetime model is appropriate for this measurement.
"""

from pathlib import Path

import numpy as np
import pytest
import tttrlib


@pytest.fixture
def measured_problem():
    """Construct a public provider problem without a ChiSurf dependency."""
    path = Path(__file__).resolve().parents[2] / "data/fit24_parameter_reporting.npz"
    with np.load(path, allow_pickle=False) as recorded:
        arrays = {key: recorded[key].copy() for key in recorded.files}
    dt = float(arrays["dt"])
    n_bins = arrays["data"].size // 2
    setup = tttrlib.setup_vector(
        "fit24",
        dt=dt,
        period=float(arrays["period"]),
        g_factor=float(arrays["g_factor"]),
        l1=float(arrays["l1"]),
        l2=float(arrays["l2"]),
        soft_bifl_scatter_flag=bool(arrays["BIFL_scatter"]),
        objective="p2s_mle" if bool(arrays["p2s_twoIstar"]) else "poisson_mle",
    )
    fit = tttrlib.DecayFit2("fit24", setup, arrays["irf"].tolist())
    problem = tttrlib.DecayFitProblem(2, n_bins, dt)
    problem.irf = tttrlib.VectorDouble(arrays["irf"].tolist())
    problem.background = tttrlib.VectorDouble(arrays["bg"].tolist())
    problem.data = tttrlib.VectorDouble(arrays["data"].astype(float).tolist())
    return fit, problem, arrays


def _assert_report_reconstructs_fit(fit, problem, result):
    """Compare public forward prediction and evaluation with the realized fit."""
    realized = np.asarray(problem.model).copy().reshape(2, -1)
    reported = list(result.parameters)
    predicted = np.asarray(fit.model_curve(reported, problem)).reshape(2, -1)
    np.testing.assert_allclose(
        predicted / predicted.sum(axis=1, keepdims=True),
        realized / realized.sum(axis=1, keepdims=True),
        rtol=1e-12,
        atol=1e-14,
    )
    # evaluate returns the minimizer's likelihood; fit reports the deviance.
    # Refit fixed reported parameters to compare the same reported statistic.
    refitted = fit.fit(
        reported,
        tttrlib.DecayFitConstraints(tttrlib.VectorInt32([-1] * 5)),
        problem,
    )
    assert refitted.objective == pytest.approx(result.objective, rel=1e-12)


@pytest.mark.parametrize(
    "slot, raw, effective",
    [
        (3, -0.1, 0.0),
        (3, 1.2, 0.999),
        (3, 0.4, 0.4),
        (1, -0.1, 0.0),
        (1, 1.2, 0.999),
        (4, -0.1, 0.0),
    ],
)
def test_fixed_projected_parameters_report_the_realized_curve(
    measured_problem,
    slot,
    raw,
    effective,
):
    """Existing input correction applies even when the caller fixes a slot."""
    fit, problem, arrays = measured_problem
    initial = arrays["x0"].copy()
    initial[slot] = raw
    constraints = tttrlib.DecayFitConstraints(tttrlib.VectorInt32([-1] * 5))
    result = fit.fit(initial.tolist(), constraints, problem)
    expected = initial.copy()
    expected[slot] = effective
    np.testing.assert_allclose(list(result.parameters), expected, rtol=0, atol=1e-14)
    _assert_report_reconstructs_fit(fit, problem, result)


def test_measured_fit_reports_the_mixture_used_for_its_unchanged_score(measured_problem):
    """A legal starting mixture can reach the projection boundary in real data."""
    fit, problem, arrays = measured_problem
    links = np.where(arrays["fixed"], -1, 0).astype(int).tolist()
    result = fit.fit(
        arrays["x0"].tolist(),
        tttrlib.DecayFitConstraints(tttrlib.VectorInt32(links)),
        problem,
    )
    assert 0.0 <= result.parameters[3] <= 1.0
    assert result.objective == pytest.approx(1.7064816013903084, rel=1e-10)
    expected = arrays["result_x"].copy()
    expected[3] = 0.0
    np.testing.assert_allclose(list(result.parameters), expected, rtol=1e-6, atol=1e-7)
    np.testing.assert_allclose(
        np.asarray(problem.model), arrays["result_curve"], rtol=1e-6, atol=1e-7
    )
    _assert_report_reconstructs_fit(fit, problem, result)
