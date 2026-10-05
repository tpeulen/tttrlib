"""A/B of the scipy replacements in `modules/math` against scipy itself.

`NdImage.h` (via `tttrlib.ndimage`), `Lsap.h` (`linear_sum_assignment`),
`Hierarchy.h` (`linkage`, `fcluster`, `inconsistent`), the k-d tree queries
(`KDTree.query`, `.query_ball_point`, `.query_pairs`) and `ConvexHull2D` exist
so that callers can drop scipy without their numbers moving. That is the claim
pinned here, and it is pinned tightly: every filter, the morphology, labelling,
the feature transform, the spline sampler, the assignment and the dendrogram
are compared for *exact* equality -- dtype included -- because each is a
line-for-line port and anything less than identity means a port defect, not
"numerical noise". The k-d tree distances and the hull area are the two
places a different (equally correct) summation is used, and are compared to
a few ulp.

scipy is the oracle and is optional: without it the whole file skips.
"""

import numpy as np
import pytest

import tttrlib

sn = pytest.importorskip("scipy.ndimage")
so = pytest.importorskip("scipy.optimize")
sh = pytest.importorskip("scipy.cluster.hierarchy")
ss = pytest.importorskip("scipy.spatial")
tn = pytest.importorskip("tttrlib.ndimage")

MODES = ["reflect", "constant", "nearest", "mirror", "wrap"]
RNG = np.random.default_rng(20261005)
IMG2 = RNG.normal(size=(23, 31))
IMG3 = RNG.normal(size=(7, 9, 11))
IMG1 = RNG.normal(size=40)
U8 = RNG.integers(0, 255, size=(19, 21)).astype(np.uint8)
MASK = RNG.random((25, 30)) > 0.6


def same(a, b):
    a, b = np.asarray(a), np.asarray(b)
    assert a.shape == b.shape
    assert a.dtype == b.dtype, (a.dtype, b.dtype)
    np.testing.assert_array_equal(a, b)


# --- separable filters ------------------------------------------------------------


@pytest.mark.parametrize("mode", MODES)
@pytest.mark.parametrize("weights", [[1, 3], [0.25, 0.5, 0.25], [1, 0, -1], [0.1, 0.4, 0.2, 0.3], [1, 2, 3, 4, 5]])
def test_correlate1d(mode, weights):
    lw = len(weights)
    for origin in range(-(lw // 2), (lw - 1) // 2 + 1):
        same(tn.correlate1d(IMG2, weights, 1, mode=mode, cval=0.7, origin=origin),
             sn.correlate1d(IMG2, weights, 1, mode=mode, cval=0.7, origin=origin))
        same(tn.convolve1d(IMG2, weights, 0, mode=mode, origin=origin),
             sn.convolve1d(IMG2, weights, 0, mode=mode, origin=origin))


def test_correlate1d_extension_longer_than_line():
    # NI_ExtendLine repeats the line periodically once the pad exceeds it
    a = RNG.normal(size=(3, 2))
    for mode in MODES:
        same(tn.correlate1d(a, np.arange(1.0, 10.0), 1, mode=mode), sn.correlate1d(a, np.arange(1.0, 10.0), 1, mode=mode))


@pytest.mark.parametrize("mode", MODES)
def test_gaussian_family(mode):
    for sigma in (0.7, 1.0, 2.5):
        for order in (0, 1, 2):
            same(tn.gaussian_filter(IMG2, sigma, order=order, mode=mode),
                 sn.gaussian_filter(IMG2, sigma, order=order, mode=mode))
    same(tn.gaussian_filter(IMG3, (1.0, 0.5, 2.0), mode=mode), sn.gaussian_filter(IMG3, (1.0, 0.5, 2.0), mode=mode))
    # integer and float32 images are written back in their dtype pass by pass
    same(tn.gaussian_filter(U8, 1.3, mode=mode), sn.gaussian_filter(U8, 1.3, mode=mode))
    f32 = IMG2.astype(np.float32)
    same(tn.gaussian_filter(f32, 1.3, mode=mode), sn.gaussian_filter(f32, 1.3, mode=mode))
    same(tn.gaussian_laplace(IMG2, 1.7, mode=mode), sn.gaussian_laplace(IMG2, 1.7, mode=mode))
    same(tn.laplace(IMG2, mode=mode), sn.laplace(IMG2, mode=mode))


@pytest.mark.parametrize("mode", MODES)
def test_box_rank_and_extremum_filters(mode):
    for size in (2, 3, 4, (3, 5)):
        same(tn.uniform_filter(IMG2, size, mode=mode), sn.uniform_filter(IMG2, size, mode=mode))
        same(tn.maximum_filter(IMG2, size, mode=mode), sn.maximum_filter(IMG2, size, mode=mode))
        same(tn.minimum_filter(IMG2, size, mode=mode), sn.minimum_filter(IMG2, size, mode=mode))
        same(tn.median_filter(IMG2, size, mode=mode), sn.median_filter(IMG2, size, mode=mode))
        same(tn.median_filter(U8, size, mode=mode), sn.median_filter(U8, size, mode=mode))
    for size in (2, 3, 5):
        same(tn.median_filter(IMG1, size, mode=mode), sn.median_filter(IMG1, size, mode=mode))
    # number_brightness's MATLAB-centred box: an origin per axis
    same(tn.uniform_filter(IMG2, (4, 3), mode=mode, origin=[-1, 0]),
         sn.uniform_filter(IMG2, (4, 3), mode=mode, origin=[-1, 0]))
    fp = np.array([[0, 1, 0], [1, 1, 1], [0, 1, 1]], bool)
    same(tn.maximum_filter(IMG2, footprint=fp, mode=mode), sn.maximum_filter(IMG2, footprint=fp, mode=mode))
    same(tn.rank_filter(IMG2, 2, footprint=fp, mode=mode), sn.rank_filter(IMG2, 2, footprint=fp, mode=mode))
    cross = sn.generate_binary_structure(3, 1)
    same(tn.median_filter(IMG3, footprint=cross, mode=mode), sn.median_filter(IMG3, footprint=cross, mode=mode))


@pytest.mark.parametrize("mode", MODES)
def test_nd_correlate_and_convolve(mode):
    k = RNG.normal(size=(3, 4))  # even axis: convolve's origin shift
    same(tn.correlate(IMG2, k, mode=mode, cval=0.3), sn.correlate(IMG2, k, mode=mode, cval=0.3))
    same(tn.convolve(IMG2, k, mode=mode, cval=0.3), sn.convolve(IMG2, k, mode=mode, cval=0.3))
    k3 = RNG.normal(size=(2, 3, 3))
    same(tn.correlate(IMG3, k3, mode=mode), sn.correlate(IMG3, k3, mode=mode))
    smooth = np.array([[1, 2, 1], [2, 4, 2], [1, 2, 1]])
    same(tn.convolve(U8, smooth, mode=mode), sn.convolve(U8, smooth, mode=mode))


def test_region_property_kernels():
    """The exact calls chisurf.core.roi.props makes, uint8 overflow and all."""
    b8 = MASK.astype(np.uint8)
    eroded = tn.binary_erosion(b8, tn.generate_binary_structure(2, 1), border_value=0)
    same(eroded, sn.binary_erosion(b8, sn.generate_binary_structure(2, 1), border_value=0))
    border = b8 - eroded.astype(np.uint8)
    perimeter = np.array([[10, 2, 10], [2, 1, 2], [10, 2, 10]])
    same(tn.convolve(border, perimeter, mode="constant", cval=0), sn.convolve(border, perimeter, mode="constant", cval=0))
    crofton = np.array([[0, 0, 0], [0, 1, 4], [0, 2, 8]])
    pad = np.pad(b8, 1)
    same(tn.convolve(pad, crofton, mode="constant", cval=0), sn.convolve(pad, crofton, mode="constant", cval=0))
    same(tn.convolve(pad.astype(int), crofton, mode="constant", cval=0),
         sn.convolve(pad.astype(int), crofton, mode="constant", cval=0))


# --- labels and measurements --------------------------------------------------------


def test_label_numbering_and_objects():
    for conn in (1, 2):
        st = sn.generate_binary_structure(2, conn)
        same(tn.generate_binary_structure(2, conn), st)
        a, n = tn.label(MASK, st)
        b, m = sn.label(MASK, st)
        same(a, b)
        assert n == m
    for conn in (1, 2, 3):
        vol = RNG.random((8, 9, 10)) > 0.65
        st = sn.generate_binary_structure(3, conn)
        same(tn.label(vol, st)[0], sn.label(vol, st)[0])
    lab = sn.label(MASK)[0]
    assert tn.find_objects(lab) == sn.find_objects(lab)
    gap = lab.copy()
    gap[gap == 3] = 0
    assert tn.find_objects(gap, max_label=lab.max() + 2) == sn.find_objects(gap, max_label=lab.max() + 2)


def test_labelled_measurements():
    lab = sn.label(MASK)[0]
    n = lab.max()
    w = RNG.random(MASK.shape)
    same(tn.sum(w, lab, index=range(1, n + 1)), sn.sum(w, lab, index=range(1, n + 1)))
    same(tn.sum(w, lab, index=2), sn.sum(w, lab, index=2))
    same(tn.mean(w, lab, range(1, n + 1)), sn.mean(w, lab, range(1, n + 1)))
    same(np.array(tn.center_of_mass(w, lab, range(1, n + 1))), np.array(sn.center_of_mass(w, lab, range(1, n + 1))))


# --- morphology ----------------------------------------------------------------------


def test_binary_morphology():
    st8 = sn.generate_binary_structure(2, 2)
    for it in (1, 2, 3):
        for bv in (0, 1):
            same(tn.binary_erosion(MASK, iterations=it, border_value=bv),
                 sn.binary_erosion(MASK, iterations=it, border_value=bv))
            same(tn.binary_dilation(MASK, iterations=it, border_value=bv),
                 sn.binary_dilation(MASK, iterations=it, border_value=bv))
            same(tn.binary_dilation(MASK, st8, iterations=it, border_value=bv),
                 sn.binary_dilation(MASK, st8, iterations=it, border_value=bv))
        same(tn.binary_opening(MASK, iterations=it), sn.binary_opening(MASK, iterations=it))
        same(tn.binary_closing(MASK, iterations=it), sn.binary_closing(MASK, iterations=it))
    asym = np.array([[1, 1, 0], [0, 1, 0], [0, 1, 1]], bool)
    same(tn.binary_dilation(MASK, asym), sn.binary_dilation(MASK, asym))
    same(tn.binary_erosion(MASK, asym), sn.binary_erosion(MASK, asym))
    same(tn.binary_dilation(MASK, np.ones((2, 2), bool)), sn.binary_dilation(MASK, np.ones((2, 2), bool)))
    gate = RNG.random(MASK.shape) > 0.3
    same(tn.binary_dilation(MASK, iterations=3, mask=gate), sn.binary_dilation(MASK, iterations=3, mask=gate))
    same(tn.binary_dilation(MASK, iterations=-1, mask=gate), sn.binary_dilation(MASK, iterations=-1, mask=gate))


def test_fill_holes():
    ring = np.zeros((20, 20), bool)
    ring[3:15, 3:15] = True
    ring[6:10, 6:12] = False
    ring[0:2, 0:2] = True
    same(tn.binary_fill_holes(ring), sn.binary_fill_holes(ring))
    same(tn.binary_fill_holes(MASK), sn.binary_fill_holes(MASK))
    vol = RNG.random((6, 7, 8)) > 0.4
    same(tn.binary_fill_holes(vol), sn.binary_fill_holes(vol))


def test_grey_morphology():
    labels = sn.label(MASK)[0].astype(np.int32)
    cross = sn.generate_binary_structure(2, 1)
    same(tn.grey_dilation(labels, footprint=cross), sn.grey_dilation(labels, footprint=cross))
    same(tn.grey_erosion(labels, footprint=cross), sn.grey_erosion(labels, footprint=cross))
    same(tn.grey_opening(IMG2, footprint=np.ones((3, 3), bool)), sn.grey_opening(IMG2, footprint=np.ones((3, 3), bool)))
    same(tn.grey_opening(IMG2, footprint=cross), sn.grey_opening(IMG2, footprint=cross))
    same(tn.grey_dilation(IMG2, size=(2, 3)), sn.grey_dilation(IMG2, size=(2, 3)))
    asym = np.array([[1, 1, 0], [0, 1, 0], [0, 1, 1]], bool)
    same(tn.grey_dilation(IMG2, footprint=asym), sn.grey_dilation(IMG2, footprint=asym))


# --- distance transform ----------------------------------------------------------------


@pytest.mark.parametrize("shape,p", [((30, 40), 0.9), ((12, 13, 14), 0.85), ((50,), 0.8), ((20, 20), 0.5)])
def test_distance_transform_edt(shape, p):
    m = RNG.random(shape) < p
    for sampling in (None, [0.5, 1.7, 1.1][: len(shape)]):
        d1, i1 = tn.distance_transform_edt(m, sampling=sampling, return_indices=True)
        d2, i2 = sn.distance_transform_edt(m, sampling=sampling, return_indices=True)
        same(d1, d2)
        same(i1, i2)


def test_distance_transform_ties():
    # an integer lattice of seeds: most pixels are equidistant from several
    m = np.ones((21, 21), bool)
    m[::5, ::5] = False
    same(tn.distance_transform_edt(m, return_indices=True)[1], sn.distance_transform_edt(m, return_indices=True)[1])


# --- splines --------------------------------------------------------------------------


@pytest.mark.parametrize("mode", ["constant", "nearest", "mirror", "reflect", "wrap", "grid-wrap", "grid-constant"])
def test_spline_interpolation(mode):
    psf = np.exp(-((np.arange(15)[:, None] - 7.2) ** 2 + (np.arange(15)[None] - 6.6) ** 2) / 4.0)
    for order in (1, 3):
        same(tn.shift(psf, (0.37, -1.6), order=order, mode=mode), sn.shift(psf, (0.37, -1.6), order=order, mode=mode))
    grids = np.mgrid[-2:16:0.7, -1:17:0.9]
    for order in (2, 3, 4, 5):
        same(tn.map_coordinates(psf, grids, order=order, mode=mode), sn.map_coordinates(psf, grids, order=order, mode=mode))
    same(tn.spline_filter(psf, 3, mode=mode), sn.spline_filter(psf, 3, mode=mode))


def test_map_coordinates_3d():
    vol = RNG.random((9, 10, 11))
    grid = np.stack(np.mgrid[0:8:0.6, 1:9:0.75, 0:10:1.1])
    same(tn.map_coordinates(vol, grid, order=3), sn.map_coordinates(vol, grid, order=3))


# --- assignment, hierarchy, k-d tree, hull -------------------------------------------------


@pytest.mark.parametrize("shape", [(6, 6), (5, 8), (8, 5), (30, 30), (1, 4)])
def test_linear_sum_assignment(shape):
    for cost in (RNG.random(shape), RNG.integers(0, 4, size=shape).astype(float)):
        for maximize in (False, True):
            r1, c1 = tttrlib.linear_sum_assignment(cost, maximize=maximize)
            r2, c2 = so.linear_sum_assignment(cost, maximize=maximize)
            same(r1, r2.astype(np.int64))
            same(c1, c2.astype(np.int64))


def test_linear_sum_assignment_special():
    r1, c1 = tttrlib.linear_sum_assignment(np.ones((5, 5)))
    r2, c2 = so.linear_sum_assignment(np.ones((5, 5)))
    same(c1, c2.astype(np.int64))
    cost = RNG.random((6, 6))
    cost[0, :3] = np.inf
    cost[2, 4] = np.inf
    same(tttrlib.linear_sum_assignment(cost)[1], so.linear_sum_assignment(cost)[1].astype(np.int64))
    infeasible = RNG.random((4, 4))
    infeasible[1, :] = np.inf
    for bad in (np.full((3, 3), np.nan), infeasible):
        with pytest.raises(ValueError):
            so.linear_sum_assignment(bad)
        with pytest.raises(ValueError):
            tttrlib.linear_sum_assignment(bad)


@pytest.mark.parametrize("method", ["single", "complete", "average", "weighted", "ward"])
def test_linkage_and_fcluster(method):
    x = RNG.normal(size=(40, 3))
    d = ss.distance.pdist(x)
    np.testing.assert_allclose(tttrlib.pdist(x), d, rtol=0, atol=1e-14)
    ties = RNG.integers(1, 6, size=d.size).astype(float)
    for dd in (d, ties):
        Z = sh.linkage(dd, method)
        same(tttrlib.linkage(dd, method), Z)
        for crit, t in (("maxclust", 4), ("maxclust", 1), ("maxclust", 100),
                        ("distance", float(np.median(Z[:, 2]))), ("inconsistent", 0.8)):
            same(tttrlib.fcluster(Z, t, crit), sh.fcluster(Z, t, crit))
        same(tttrlib.inconsistent(Z, 3), sh.inconsistent(Z, 3))
        mc = sh.maxdists(Z)
        same(tttrlib.fcluster(Z, 0.5, "monocrit", monocrit=mc), sh.fcluster(Z, 0.5, "monocrit", monocrit=mc))
        same(tttrlib.fcluster(Z, 3, "maxclust_monocrit", monocrit=mc),
             sh.fcluster(Z, 3, "maxclust_monocrit", monocrit=mc))


def test_kdtree_queries():
    pts = RNG.random((300, 2)) * 10
    ours, theirs = tttrlib.KDTree(pts), ss.cKDTree(pts)
    q = RNG.random((50, 2)) * 10
    for k in (1, 3, [1, 3]):
        d1, i1 = ours.query(q, k=k)
        d2, i2 = theirs.query(q, k=k)
        np.testing.assert_array_equal(i1, i2)
        np.testing.assert_allclose(d1, d2, rtol=1e-15, atol=0)
    d1, i1 = ours.query(q[0])
    d2, i2 = theirs.query(q[0])
    assert i1 == i2 and abs(d1 - d2) <= 1e-15
    d1, i1 = ours.query(q[:3], k=400)  # more neighbours than points: inf / n padding
    d2, i2 = theirs.query(q[:3], k=400)
    np.testing.assert_array_equal(i1, i2)
    np.testing.assert_allclose(d1, d2, rtol=1e-15, atol=0)
    for p in (2.0, 1.0, np.inf, 3.0):
        a, b = ours.query_ball_point(q, 1.3, p=p), theirs.query_ball_point(q, 1.3, p=p)
        assert all(sorted(x) == sorted(y) for x, y in zip(a, b))
        assert sorted(ours.query_ball_point(q[5], 1.3, p=p)) == sorted(theirs.query_ball_point(q[5], 1.3, p=p))
        assert ours.query_pairs(0.6, p=p) == theirs.query_pairs(0.6, p=p)


@pytest.mark.parametrize("points", [
    RNG.random((60, 2)),
    np.array([[0, 0], [1, 0], [2, 0], [2, 1], [2, 2], [0, 2], [1, 1], [0, 1], [0, 0.0]]),  # collinear + duplicate
])
def test_convex_hull_2d(points):
    ours, theirs = tttrlib.ConvexHull2D(points), ss.ConvexHull(points)
    assert set(ours.vertices.tolist()) == set(theirs.vertices.tolist())
    np.testing.assert_allclose(ours.volume, theirs.volume, rtol=1e-14)
    np.testing.assert_allclose(ours.area, theirs.area, rtol=1e-14)
    e1 = sorted(map(tuple, np.round(ours.equations, 12)))
    e2 = sorted(map(tuple, np.round(theirs.equations, 12)))
    np.testing.assert_allclose(e1, e2, atol=1e-12)
    inside = np.all(points @ ours.equations[:, :2].T + ours.equations[:, 2] <= 1e-12, axis=1)
    assert inside.all()
