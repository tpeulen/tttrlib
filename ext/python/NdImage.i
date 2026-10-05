// SPDX-License-Identifier: BSD-3-Clause
%{
#include "NdImageOps.h"
#include "Lsap.h"
#include "Hierarchy.h"
#include "ConvexHull.h"
#include "SpatialOps.h"
%}

// The scipy-replacement kernels (ndimage filters/morphology/labels/EDT/
// splines, linear_sum_assignment, hierarchical clustering, the 2-D convex
// hull and k-d tree batch queries), as flat array-in/array-out entry points.
// The scipy-shaped API is `tttrlib.ndimage` (ext/python/ndimage_module.py)
// plus the thin helpers at the bottom of this file; the `ndi_*`, `lsap_*`,
// `hierarchy_*`, `kdtree_*` names are its plumbing, not an API.

// ---- inputs ---------------------------------------------------------------
%apply (double* IN_ARRAY1, int DIM1) {(const double* src, int n_src)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* weights, int n_weights)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* sampling, int n_sampling)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* coords, int n_coords)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* shifts, int n_shifts)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* condensed, int n_condensed)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* Z, int n_Z)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* R, int n_R)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* monocrit, int n_monocrit)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* shape, int ndim)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* wshape, int wndim)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* origins, int n_origins)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* labels, int n_labels)}
%apply (unsigned char* IN_ARRAY1, int DIM1) {(const unsigned char* bsrc, int n_bsrc)}
%apply (unsigned char* IN_ARRAY1, int DIM1) {(const unsigned char* footprint, int n_footprint)}
%apply (unsigned char* IN_ARRAY1, int DIM1) {(const unsigned char* mask, int n_mask)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* cost, int n_rows, int n_cols)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* xy, int n_rows, int n_cols)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* points, int n_points, int n_dims)}

// ---- outputs --------------------------------------------------------------
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** dst, int* dst_n)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out, int* n_out)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_dist, int* n_out_dist)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_equations, int* n_out_equations)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** ldst, int* ldst_n)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out, int* n_out)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_index, int* n_out_index)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_offsets, int* n_out_offsets)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_hits, int* n_out_hits)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_pairs, int* n_out_pairs)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_vertices, int* n_out_vertices)}
%apply (unsigned char** ARGOUTVIEWM_ARRAY1, int* DIM1) {(unsigned char** bdst, int* bdst_n)}
%apply (int** ARGOUTVIEWM_ARRAY1, int* DIM1) {(int** idst, int* idst_n)}
%apply (int** ARGOUTVIEWM_ARRAY1, int* DIM1) {(int** out, int* n_out)}
%apply (long long* OUTPUT) {(long long* n_found)}
%apply (int* OUTPUT) {(int* changed)}
%apply (double* OUTPUT) {(double* area)}
%apply (double* OUTPUT) {(double* perimeter)}

%exception {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    }
}

// the vector-returning C++ API stays C++; Python gets the flat entry points
%ignore tttrlib::ndimage_ops::detail::alloc;
%ignore tttrlib::ndimage_ops::detail::to_shape;
%ignore tttrlib::ndimage_ops::detail::check_size;
%ignore tttrlib::ndimage_ops::detail::to_origins;
%ignore tttrlib::linear_sum_assignment;
%ignore tttrlib::hierarchy_linkage;
%ignore tttrlib::hierarchy_inconsistent;
%ignore tttrlib::hierarchy_fcluster;
%ignore tttrlib::ConvexHull2D;
%ignore tttrlib::convex_hull_2d;
%ignore tttrlib::spatial_detail::alloc;
%include "NdImageOps.h"
%include "Lsap.h"
%include "Hierarchy.h"
%include "ConvexHull.h"
%include "SpatialOps.h"
%exception;

#ifdef SWIGPYTHON
%pythoncode %{
def linear_sum_assignment(cost_matrix, maximize=False):
    """Solve the linear sum assignment problem (scipy.optimize semantics).

    Parameters
    ----------
    cost_matrix : array_like, shape (n, m)
        Cost of assigning row ``i`` to column ``j``; ``+inf`` forbids a pair.
    maximize : bool
        Maximise the summed cost instead of minimising it.

    Returns
    -------
    row_ind, col_ind : ndarray of int64
        The ``min(n, m)`` matched pairs, ``row_ind`` ascending. With tied
        costs the pairs are the ones scipy returns, not merely an optimum.
    """
    a = np.ascontiguousarray(np.asarray(cost_matrix, dtype=np.float64))
    if a.ndim != 2:
        raise ValueError("expected a matrix (2-D array), got a %d-D array" % a.ndim)
    if a.size == 0:
        return np.zeros(0, dtype=np.int64), np.zeros(0, dtype=np.int64)
    flat = lsap_solve(a, int(bool(maximize)))
    n = flat.size // 2
    return np.asarray(flat[:n], dtype=np.int64), np.asarray(flat[n:], dtype=np.int64)


_LINKAGE_METHODS = {"single": 0, "complete": 1, "average": 2, "ward": 5, "weighted": 6}


def linkage(y, method="single"):
    """Agglomerative clustering of a condensed distance matrix (scipy.cluster.hierarchy).

    Parameters
    ----------
    y : array_like
        Condensed distances, ``n * (n - 1) / 2`` of them in ``pdist`` order.
    method : str
        ``single``, ``complete``, ``average`` (UPGMA), ``weighted`` (WPGMA)
        or ``ward``.

    Returns
    -------
    ndarray, shape (n - 1, 4)
        scipy's linkage matrix: merged ids, merge distance, new cluster size.
    """
    try:
        code = _LINKAGE_METHODS[method]
    except KeyError:
        raise ValueError("Invalid method: %r (centroid and median are not implemented)" % (method,))
    d = np.ascontiguousarray(np.asarray(y, dtype=np.float64).ravel())
    return hierarchy_linkage_flat(d, code).reshape(-1, 4)


def inconsistent(Z, d=2):
    """Inconsistency statistics of a linkage matrix, ``d`` levels deep: ``(n - 1, 4)``."""
    z = np.ascontiguousarray(np.asarray(Z, dtype=np.float64).ravel())
    return hierarchy_inconsistent_flat(z, int(d)).reshape(-1, 4)


def fcluster(Z, t, criterion="inconsistent", depth=2, R=None, monocrit=None):
    """Flat clusters from a linkage matrix (scipy.cluster.hierarchy.fcluster).

    Parameters
    ----------
    Z : array_like, shape (n - 1, 4)
        Linkage matrix.
    t : scalar
        Threshold (distance/inconsistency/monocrit) or the maximum number of
        clusters (maxclust, maxclust_monocrit).
    criterion : str
        ``inconsistent``, ``distance``, ``maxclust``, ``monocrit`` or
        ``maxclust_monocrit``.

    Returns
    -------
    ndarray of int32
        1-based cluster number of every observation, numbered as scipy does.
    """
    z = np.ascontiguousarray(np.asarray(Z, dtype=np.float64).ravel())
    r = np.zeros(0) if R is None else np.ascontiguousarray(np.asarray(R, dtype=np.float64).ravel())
    m = np.zeros(0) if monocrit is None else np.ascontiguousarray(np.asarray(monocrit, dtype=np.float64).ravel())
    if criterion in ("maxclust", "maxclust_monocrit"):
        t = int(t)
    return np.asarray(hierarchy_fcluster_flat(z, str(criterion), float(t), int(depth), r, m), dtype=np.int32)


class ConvexHull2D(object):
    """Convex hull of 2-D points, shaped like ``scipy.spatial.ConvexHull``.

    Attributes
    ----------
    points : ndarray, (n, 2)
    vertices : ndarray of int64
        Indices of the hull vertices in counterclockwise order.
    equations : ndarray, (n_edges, 3)
        ``[nx, ny, offset]`` with unit outward normals: ``n . x + offset <= 0``
        inside.
    volume : float
        Enclosed area (scipy's naming in 2-D).
    area : float
        Perimeter (scipy's naming in 2-D).
    """

    def __init__(self, points):
        p = np.ascontiguousarray(np.asarray(points, dtype=np.float64))
        if p.ndim != 2 or p.shape[1] != 2:
            raise ValueError("ConvexHull2D needs an (n, 2) array of points")
        vertices, equations, area, perimeter = convex_hull_2d_flat(p)
        self.points = p
        self.vertices = np.asarray(vertices, dtype=np.int64)
        self.equations = np.asarray(equations).reshape(-1, 3)
        self.volume = float(area)
        self.area = float(perimeter)


def pdist(X):
    """Condensed Euclidean distances between the rows of ``X`` (scipy.spatial.distance.pdist order)."""
    x = np.asarray(X, dtype=np.float64)
    if x.ndim != 2:
        raise ValueError("pdist needs a 2-D array")
    i, j = np.triu_indices(x.shape[0], k=1)
    return np.sqrt(((x[i] - x[j]) ** 2).sum(axis=1))
%}
#endif
