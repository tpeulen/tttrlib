# SPDX-License-Identifier: BSD-3-Clause
"""The accurate-FRET calibration is discoverable through the algorithm registry."""
import json

import tttrlib


def _entry():
    return json.loads(tttrlib.algorithms_json("corrections"))["accurate_fret"]


def test_entry_is_registered_and_replayable():
    e = _entry()
    assert e["operation_type"] == "calibration" and e["method"] == "auto_calibrate"
    assert e["can_replay"] and len(e["references"]) >= 2
    assert "accurate_fret" in json.loads(tttrlib.registry_category_json("operation"))


def test_schema_covers_every_option_auto_calibrate_reads():
    props = _entry()["params_schema"]["properties"]
    assert set(tttrlib._AFRET_OPTION_KEYS) | {"dimensions"} == set(props)
    assert set(props["dimensions"]["items"]["enum"]) == set(tttrlib.AFRET_DIMENSIONS)
    defaults = tttrlib.AutoCalibrateOptions()
    for key, spec in props.items():
        if key != "dimensions":
            assert getattr(defaults, key) == spec["default"], key


def test_every_listed_api_function_exists():
    for name in _entry()["api"]:
        assert callable(getattr(tttrlib, name)), name
