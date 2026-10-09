"""Finite calibration values do not establish identification or adoption."""

from __future__ import annotations

import copy

import pytest
import tttrlib


def retained_result():
    """Return the unidentified finite fallback seen in a measured burst table."""
    return {
        "factors": {"alpha": 0.7084614953197103, "delta": 0.0, "gamma": 1.0,
                    "beta": 0.13462724315810634},
        "uncertainties": {},
        "gamma_estimates": {"es": float("nan"), "lifetime": float("nan"),
                            "prior": float("nan"), "data": 1.0, "posterior": 1.0},
        "estimated": {"alpha": False, "delta": False, "gamma": False},
        "messages": [],
        "converged": True,
        "iterations": 3,
    }


@pytest.mark.parametrize(
    ("identification", "label"),
    [(False, "not identified; retained value"), (True, "identified estimate"),
     (None, "identification unknown")],
)
def test_data_gamma_label_follows_declared_identification(identification, label):
    """The same finite value has three distinct, explicitly declared statuses."""
    result = retained_result()
    result["estimated"]["gamma"] = identification
    report = tttrlib.calibration_report(result)
    assert f"gamma [data ({label})] = 1.0000" in report
    assert "adopted" not in report
    assert "gamma [posterior] = 1.0000" in report


@pytest.mark.parametrize("metadata", ["missing", None, {}, {"alpha": True}])
def test_legacy_result_without_gamma_flag_has_unknown_identification(metadata):
    """Legacy payloads remain printable without inventing data identification."""
    result = retained_result()
    if metadata == "missing":
        result.pop("estimated")
    else:
        result["estimated"] = metadata
    report = tttrlib.calibration_report(result)
    assert "gamma [data (identification unknown)] = 1.0000" in report
    assert "adopted" not in report


def test_identified_data_candidate_does_not_claim_posterior_adoption():
    """An identified estimate can differ from the adopted posterior under a prior."""
    result = retained_result()
    result["estimated"]["gamma"] = True
    result["gamma_estimates"].update(data=0.8, prior=1.2, posterior=1.0)
    report = tttrlib.calibration_report(result)
    assert "gamma [data (identified estimate)] = 0.8000" in report
    assert "gamma [light path] = 1.2000" in report
    assert "gamma [posterior] = 1.0000" in report
    assert "adopted" not in report


def test_formatting_retains_payload_values_and_other_report_lines():
    """Provenance formatting leaves factors, warnings, uncertainties and results intact."""
    result = retained_result()
    result["gamma_estimates"] = {"data": 1.0, "posterior": 1.0}
    result["uncertainties"]["gamma"] = 0.2
    result["messages"] = ["gamma could not be determined from the data; prior value kept"]
    before = copy.deepcopy(result)
    report = tttrlib.calibration_report(result, ["seeded by hand"])
    assert result == before
    assert "gamma  = 1.0000 ± 0.2000" in report
    assert "! seeded by hand" in report
    assert "! gamma could not be determined from the data; prior value kept" in report
    assert "converged after 3 iteration(s)" in report


@pytest.mark.parametrize("flag", [False, True, None])
def test_nonfinite_gamma_candidate_stays_omitted(flag):
    """Identification metadata never manufactures a finite data candidate."""
    result = retained_result()
    result["estimated"]["gamma"] = flag
    result["gamma_estimates"]["data"] = float("nan")
    assert "gamma [data" not in tttrlib.calibration_report(result)
