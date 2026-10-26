"""Container addresses and file links, as tttrlib takes them.

Data inside a container is addressed as a path -- ``run.pto/m001.ptu`` -- by
ptolib's rule: the container is the shortest leading prefix that is an existing
regular file. ``data/ptolib/address_cases.json`` is ptolib's conformance table
(ptolib commit 160a036), the same one ptolib and ChiSurf run; tttrlib's readers
go through ``subfile_path``/``subfile_selector``, which delegate to it.

A file link is a member whose bytes stay in their own file beside the container
(relative path, size, SHA-256). Reading through one must give exactly what
reading the file directly gives -- that is the point of linking instead of
copying many TTTR files into a project.
"""
import glob
import json
import os
import shutil
from pathlib import Path

import numpy as np
import pytest

import tttrlib

CASES = json.loads(
    (Path(__file__).resolve().parents[1] / "data" / "ptolib" / "address_cases.json").read_text()
)["cases"]

ARRAYS = ("macro_times", "micro_times", "routing_channels", "event_types")


def _ptu():
    hits = sorted(glob.glob("tttr-data/**/*.ptu", recursive=True))
    if not hits:
        pytest.skip("no PTU test data")
    return hits[0]


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_the_shared_address_table(case, tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    for d in case.get("dirs", []):
        Path(d).mkdir(parents=True, exist_ok=True)
    for f in case.get("files", []):
        Path(f).parent.mkdir(parents=True, exist_ok=True)
        Path(f).write_bytes(b"x")
    a = tttrlib.split_address(case["address"])
    if case["container"] is None:
        assert not a
    else:
        assert a
        assert Path(a.container) == Path(case["container"])
        assert a.member == case["member"]
        container, member = a
        assert member == case["member"]


def test_a_photon_member_is_read_by_its_address(tmp_path):
    src = _ptu()
    run = str(tmp_path / "run.pto")
    f = tttrlib.PtoFile()
    assert f.create(run, "t")
    assert f.attach(src, "m001.ptu")
    assert f.commit()
    f.close()
    direct = tttrlib.TTTR(src)
    inside = tttrlib.TTTR(run + "/m001.ptu")
    assert len(inside) == len(direct)
    for name in ARRAYS:
        np.testing.assert_array_equal(np.asarray(getattr(inside, name)),
                                      np.asarray(getattr(direct, name)))


def test_a_pipe_is_not_a_selector(tmp_path):
    src = _ptu()
    run = str(tmp_path / "run.pto")
    f = tttrlib.PtoFile()
    f.create(run, "t")
    f.attach(src, "m001.ptu")
    f.commit()
    f.close()
    assert not tttrlib.split_address(run + "|m001.ptu")


def _linked_project(tmp_path):
    """A project with the PTU linked (not copied), the PTU beside it."""
    src = _ptu()
    data = tmp_path / "data"
    data.mkdir()
    ptu = data / "m000.ptu"
    shutil.copyfile(src, ptu)
    proj = tmp_path / "proj.pto"
    f = tttrlib.PtoFile()
    assert f.create(str(proj), "project")
    uid = f.add_link(str(ptu), "m000.ptu")
    assert uid, f.error()
    assert f.commit()
    f.close()
    return proj, ptu, uid


def test_reading_through_a_link_equals_reading_the_file(tmp_path):
    proj, ptu, _ = _linked_project(tmp_path)
    assert proj.stat().st_size < ptu.stat().st_size, "a link must not copy the photons"
    direct = tttrlib.TTTR(str(ptu))
    linked = tttrlib.TTTR(str(proj) + "/m000.ptu")
    assert len(linked) == len(direct)
    for name in ARRAYS:
        np.testing.assert_array_equal(np.asarray(getattr(linked, name)),
                                      np.asarray(getattr(direct, name)))


def test_a_link_records_path_size_and_hash(tmp_path):
    proj, ptu, uid = _linked_project(tmp_path)
    f = tttrlib.PtoFile()
    assert f.open(str(proj))
    assert f.is_link(uid)
    info = f.link(uid)
    assert info.path == os.path.join("data", "m000.ptu")
    assert info.size == ptu.stat().st_size
    assert info.sha256 == tttrlib.sha256_file(str(ptu))
    f.close()


def test_verify_reports_a_missing_target_and_relink_repairs_it(tmp_path):
    proj, ptu, uid = _linked_project(tmp_path)
    moved = tmp_path / "elsewhere"
    moved.mkdir()
    shutil.move(str(ptu), moved / "m000.ptu")

    f = tttrlib.PtoFile()
    assert f.open(str(proj), True)
    checks = list(f.verify_links())
    assert [(c.uid, tttrlib.link_status_name(c.status)) for c in checks] == [(uid, "missing")]
    assert f.resolve_link(uid) == ""

    candidates = list(f.find_link_candidates(uid, str(tmp_path)))
    assert candidates and Path(candidates[0].path).name == "m000.ptu"
    assert f.relink(uid, candidates[0].path)
    assert f.commit()
    f.close()
    assert len(tttrlib.TTTR(str(proj) + "/m000.ptu")) > 0


def test_relink_refuses_a_different_file_unless_forced(tmp_path):
    proj, ptu, uid = _linked_project(tmp_path)
    other = tmp_path / "other.ptu"
    other.write_bytes(ptu.read_bytes()[:-1] + b"\0")  # same size, different bytes
    f = tttrlib.PtoFile()
    assert f.open(str(proj), True)
    assert not f.relink(uid, str(other))
    assert f.relink(uid, str(other), True)
    assert f.link(uid).sha256 == tttrlib.sha256_file(str(other))
    f.close()


def test_the_mmfdb_sidecar_is_a_link(tmp_path):
    src = _ptu()
    data = tmp_path / "m000.ptu"
    shutil.copyfile(src, data)
    run = tmp_path / "m000.pto"
    f = tttrlib.PtoFile()
    assert f.create(str(run), "t")
    uid = f.add_sidecar_file("photons", "ptu", "m000.ptu", str(data))
    assert uid and f.is_link(uid)
    names = {t.name for t in f.tags_for(uid)}
    assert {"_mmfdb_artifact.file_path", "_mmfdb_artifact.is_sidecar"} <= names
    assert f.commit()
    f.close()
    assert len(tttrlib.TTTR(str(run) + "/m000.ptu")) == len(tttrlib.TTTR(str(data)))
