"""Filtered-FCS species filters: legacy .jef reader, .ffcs.json round trip,
per-photon weights, and the species cross-correlation helper."""
import struct

import numpy as np
import pytest

import tttrlib


def _legacy_jef(path, f1, f2, a1, a2):
    """Write a .jef in the SoftCorr/FLF layout (big-endian)."""
    ns, nb = f1.shape
    with open(path, "wb") as fh:
        fh.write(struct.pack(">3i", 1, ns, nb))
        fh.write(np.asarray(f1, ">f8").tobytes())
        fh.write(struct.pack(">i", ns) + struct.pack(">%dd" % ns, *a1))
        fh.write(struct.pack(">2i", ns, nb))
        fh.write(np.asarray(f2, ">f8").tobytes())
        fh.write(struct.pack(">i", ns) + struct.pack(">%dd" % ns, *a2))


def test_legacy_jef_layout_and_round_trip(tmp_path):
    rng = np.random.default_rng(1)
    f1, f2 = rng.normal(size=(3, 4 * 16)), rng.normal(size=(3, 4 * 16))
    p = tmp_path / "x.jef"
    _legacy_jef(p, f1, f2, [1.0, 2.0, 3.0], [4.0, 5.0, 6.0])
    F = tttrlib.FfcsFilter.from_legacy_jef(str(p), micro_time_binning=2)
    assert F.channels == [0, 1, 2, 3, 4, 5, 6, 7]
    np.testing.assert_array_equal(F.filters[1], f1[:, :16])     # first segment = first channel of group 1
    np.testing.assert_array_equal(F.filters[2], f1[:, 48:])
    np.testing.assert_array_equal(F.filters[5], f2[:, :16])
    assert F.metadata["species_photon_numbers"] == [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
    F.save(str(tmp_path / "x.ffcs.json"))
    G = tttrlib.FfcsFilter.load(str(tmp_path / "x.ffcs.json"))
    for c in F.channels:
        np.testing.assert_array_equal(F.filters[c], G.filters[c])
    assert G.micro_time_binning == 2


def test_weights_follow_channel_and_binned_micro_time():
    F = tttrlib.FfcsFilter(["a", "b"], {0: [[1, 2], [3, 4]], 1: [[5, 6], [7, 8]]}, micro_time_binning=10)
    w = F.weights([0, 15, 9, 19, 25], [0, 0, 1, 1, 0])
    np.testing.assert_array_equal(w, [[1, 2, 5, 6, 0], [3, 4, 7, 8, 0]])   # bin 2 is past the filter: 0


def test_identity_of_filters_made_from_patterns():
    # filters F = (M^T D^-1 M)^-1 M^T D^-1 against their own patterns: F @ M = 1
    rng = np.random.default_rng(2)
    M = rng.random((40, 2)); M /= M.sum(0)
    D = M @ [100.0, 50.0]
    F = np.linalg.solve(M.T @ (M / D[:, None]), (M / D[:, None]).T)
    filt = tttrlib.FfcsFilter(["a", "b"], {0: F[:, :20], 1: F[:, 20:]},
                              patterns={0: M[:20].T, 1: M[20:].T})
    np.testing.assert_allclose(filt.identity_check(), np.eye(2), atol=1e-12)


def test_species_cross_correlation_runs_on_streams():
    class _H:
        macro_time_resolution = 1e-8

    class _T:
        def __init__(self, n, seed):
            r = np.random.default_rng(seed)
            self.macro_times = np.cumsum(r.integers(1, 50, n)).astype(np.uint64)
            self.micro_times = r.integers(0, 20, n)
            self.routing_channels = r.integers(0, 2, n)
            self.header = _H()

    F = tttrlib.FfcsFilter(["a", "b"], {0: np.ones((2, 2)), 1: np.ones((2, 2))}, micro_time_binning=10)
    x, y = tttrlib.ffcs_correlation([_T(5000, 1), _T(5000, 2)], F, (0, 1), channel_pairs=((0, 1),),
                                    n_bins=4, n_casc=10)
    assert x.size == y.size and np.all(np.isfinite(y[1:]))
