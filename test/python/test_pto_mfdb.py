"""The PTO.MFDB layer over PtoFile: tags, lineage, verified blobs, lock, interleave."""
import numpy as np
import pytest

import tttrlib


def _container(tmp_path):
    path = str(tmp_path / "m.pto")
    f = tttrlib.PtoFile()
    assert f.create(path, "test")
    return path, f


def test_blob_is_described_and_read_back_verified(tmp_path):
    path, f = _container(tmp_path)
    source = f.add("tttr_photon_stream", "ptu", "m.ptu", b"photons")
    uid = tttrlib.pto_add_blob(f, "calibration_data", "json", "cal", b'{"g": 1}',
                               operation_type="calibration", mime_type="application/json",
                               derived_from=source, software="ndxplorer 1.2")
    assert f.commit()
    f.close()

    f = tttrlib.PtoFile()
    assert f.open(path, False)
    assert tttrlib.pto_read_blob(f, uid) == b'{"g": 1}'
    assert tttrlib.pto_tag(f, uid, "_mmfdb_operation.operation_type") == "calibration"
    assert tttrlib.pto_tag(f, uid, "_mmfdb_operation.software_version") == "1.2"
    assert tttrlib.pto_tag(f, uid, "_mmfdb_artifact.size_bytes") == 8
    assert tttrlib.pto_tag(f, uid, "no.such_item", default=None) is None
    assert tttrlib.pto_tag(f, uid, "_mmfdb_operation.settings_hash") == \
        tttrlib.pto_settings_hash(None)
    assert tttrlib.pto_parents(f, uid) == [source]
    f.close()


def test_describing_twice_records_a_parent_once(tmp_path):
    _, f = _container(tmp_path)
    parent = f.add("tttr_photon_stream", "ptu", "m.ptu", b"x")
    uid = f.add("burst_table", "dstore", "b", b"y")
    tttrlib.pto_describe(f, uid, derived_from=[parent])
    tttrlib.pto_describe(f, uid, derived_from=[parent])
    assert tttrlib.pto_parents(f, uid) == [parent]
    ids = [t for t in f.tags_for(uid) if t.name == "_mmfdb_artifact.artifact_id"]
    assert len(ids) == 1


def test_a_corrupted_blob_is_refused(tmp_path):
    _, f = _container(tmp_path)
    uid = tttrlib.pto_add_blob(f, "calibration_data", "json", "cal", b"abc")
    assert f.update(uid, b"abd")
    with pytest.raises(RuntimeError, match="checksum"):
        tttrlib.pto_read_blob(f, uid)
    assert tttrlib.pto_read_blob(f, uid, verify=False) == b"abd"


def test_a_second_writer_is_told_who_holds_the_lock(tmp_path):
    path = str(tmp_path / "m.pto")
    with tttrlib.PtoWriteLock(path):
        with pytest.raises(tttrlib.PtoLockedError, match="open for writing"):
            tttrlib.PtoWriteLock(path).acquire()
    tttrlib.PtoWriteLock(path).acquire().release()


def test_the_bur_interleave_and_blank_column_are_dropped():
    store = tttrlib.DataStore()
    store.add("Number of Photons", np.array([0, 5, 0, 7, 0], dtype=float))
    store.add("Duration (ms)", np.array([0, 1.5, 0, 2.5, 0]))
    store.add(" ", np.zeros(5))
    out = tttrlib.deinterleave_burst_rows(store)
    assert out.n_rows() == 2 and out.n_columns() == 2
    assert list(out.column(0).numpy()) == [5, 7]
    assert store.n_rows() == 5 and store.n_columns() == 3   # the input is untouched


def test_a_table_that_is_not_interleaved_comes_back_unchanged():
    store = tttrlib.DataStore()
    store.add("x", np.array([1.0, 0.0, 2.0]))
    assert tttrlib.deinterleave_burst_rows(store) is store
