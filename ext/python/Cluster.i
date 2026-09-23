// SPDX-License-Identifier: BSD-3-Clause
%{
#include "Cluster.h"
#include "KMeans.h"
%}

// The sample table arrives as a 2D NumPy array (rows = samples).
%apply(double* IN_ARRAY2, int DIM1, int DIM2) {(const double* data, int n_samples, int n_features)}

// The post-MST half: edge lists in, trees and labels out. Same typemap names
// work in r/java/js (rarrays.i, jarrays.i, jsarrays.i), so one %apply serves
// every binding that includes this file.
%apply (long long* IN_ARRAY1, int DIM1) {(long long* sources, int n_sources)}
%apply (long long* IN_ARRAY1, int DIM1) {(long long* targets, int n_targets)}
%apply (double* IN_ARRAY1, int DIM1) {(double* weights, int n_weights)}
%apply (long long* IN_ARRAY1, int DIM1) {(long long* parents, int n_parents)}
%apply (long long* IN_ARRAY1, int DIM1) {(long long* children, int n_children)}
%apply (unsigned char* IN_ARRAY1, int DIM1) {(unsigned char* is_selected, int n_is_selected)}
%apply (double* IN_ARRAY1, int DIM1) {(double* lambdas, int n_lambdas)}
%apply (long long* IN_ARRAY1, int DIM1) {(long long* sizes, int n_sizes)}
%apply (long long* IN_ARRAY1, int DIM1) {(long long* roots, int n_roots)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_parent, int* n_out_parent)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_child, int* n_out_child)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_value, int* n_out_value)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_size, int* n_out_size)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out, int* n_out)}
%apply (unsigned char** ARGOUTVIEWM_ARRAY1, int* DIM1) {(unsigned char** out_selected, int* n_out_selected)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_strength, int* n_out_strength)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_stability, int* n_out_stability)}

// Both kernels are long-running and touch no Python object.
TTTRLIB_NOGIL(tttrlib::hdbscan_condensed_tree)
TTTRLIB_NOGIL(tttrlib::hdbscan_select_clusters)
TTTRLIB_NOGIL(tttrlib::hdbscan_label_points)
TTTRLIB_NOGIL(tttrlib::hdbscan_cluster_stability)
TTTRLIB_NOGIL(tttrlib::hdbscan_membership_strengths)
TTTRLIB_NOGIL(tttrlib::core_distances)
TTTRLIB_NOGIL(tttrlib::mutual_reachability_mst)
TTTRLIB_NOGIL(tttrlib::KDTree::core_distances)
TTTRLIB_NOGIL(tttrlib::KDTree::mutual_reachability_mst)

// k-means: the sample table typemap above serves X; the uniforms arrive as a
// 1D array and everything the fit produces comes back in ARGOUTVIEWM buffers.
%apply (double* IN_ARRAY1, int DIM1) {(const double* uniforms, int n_uniforms)}
%apply (double** ARGOUTVIEWM_ARRAY2, int* DIM1, int* DIM2) {(double** out_centers, int* out_n1, int* out_n2)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_labels, int* out_n_labels)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_stats, int* out_n_stats)}
TTTRLIB_NOGIL(tttrlib::kmeans)

%exception {
    try {
        $action
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    }
}

// `query` writes into caller-supplied buffers, which has no useful Python
// spelling; the Python-facing surface is the two free functions plus the
// tree's own core_distances / mutual_reachability_mst.
%ignore tttrlib::KDTree::query;

%include "Cluster.h"
%include "KMeans.h"

#ifdef SWIGPYTHON
%extend tttrlib::KDTree {
    %pythoncode %{
    def __repr__(self):
        return "KDTree({} x {})".format(self.n_samples(), self.n_features())
    %}
}

// The k-means++ seeding consumes n_init * n_clusters * (2 + int(ln k)) uniforms
// (the greedy variant's trial count), which every caller was re-deriving. The
// randomness stays the caller's: these only size and, on request, draw it.
%pythoncode %{
import math as _math

def kmeans_n_uniforms(n_clusters, n_init=1):
    """Number of uniforms `kmeans` consumes: n_init * n_clusters * (2 + int(ln n_clusters))."""
    return int(n_init) * int(n_clusters) * (2 + int(_math.log(int(n_clusters))))


def kmeans_uniforms(n_clusters, n_init=1, seed=None):
    """A stream of the right length from ``numpy.random.default_rng(seed)``.

    The kernel never draws randomness itself; passing the same ``seed`` here
    reproduces a fit exactly, which is the contract ChiSurf relies on.
    """
    import numpy as _np
    return _np.random.default_rng(seed).random(kmeans_n_uniforms(n_clusters, n_init))
%}

// The five-line pipeline every caller writes: MST, condense, select, label,
// score. The kernels stay separate because a caller with its own selection
// policy has to be able to step in between them; this is for the caller who
// does not.
%pythoncode %{
import collections as _collections

HdbscanResult = _collections.namedtuple(
    "HdbscanResult", ("labels", "probabilities", "persistence"))


def hdbscan(x, min_cluster_size=5, min_samples=None, alpha=1.0,
            cluster_selection_method="eom", allow_single_cluster=False,
            cluster_selection_epsilon=0.0, max_cluster_size=0):
    """Density-based clustering of an ``(n_samples, n_features)`` table.

    HDBSCAN of Campello, Moulavi & Sander (PAKDD 2013): the whole pipeline —
    ``core_distances`` → ``mutual_reachability_mst`` →
    ``hdbscan_condensed_tree`` → ``hdbscan_select_clusters`` →
    ``hdbscan_label_points`` → ``hdbscan_membership_strengths``. Call those
    directly to substitute your own cluster-selection policy; this is the
    standard composition of them.

    Parameters mean what they mean in ``sklearn.cluster.HDBSCAN``, whose labels
    this reproduces on the same spanning tree:

    x
        Row per sample. Scale the columns first — the distance is Euclidean, so
        an unstandardised feature with a wide range decides the clustering.
    min_cluster_size
        Fewer points than this is not a cluster but a fragment falling out of
        one.
    min_samples
        Neighbours the core distance is measured to, i.e. how conservative the
        density estimate is. Defaults to ``min_cluster_size``.
    alpha
        Divides the plain distance before the mutual-reachability inflation;
        above one makes the hierarchy more conservative.
    cluster_selection_method
        ``"eom"`` (excess of mass, the default: few, stable clusters) or
        ``"leaf"`` (every density peak, so more and smaller ones).
    allow_single_cluster
        Admit the answer "this is one cluster". Off by default: with it on,
        structureless data comes back as one cluster instead of as noise.
    cluster_selection_epsilon
        Distance below which splits are not taken, in the units of the data
        (Malzer & Baum 2020). ``0.0`` disables it.
    max_cluster_size
        Points; a candidate above it loses to its descendants (``"eom"`` only).
        ``0`` disables it.

    Returns
    -------
    HdbscanResult
        ``labels``, one per row, ``-1`` for noise; ``probabilities``, the
        membership strength in ``[0, 1]`` (``0`` for noise) to threshold on when
        the marginal points matter; and ``persistence``, one value per cluster
        in ``[0, 1]``.

        ``probabilities`` is a rank *within* a cluster, not a confidence across
        clusters: the denominator is each cluster's own death, so every cluster
        reaches one somewhere however diffuse it is. Do not carry a threshold
        between the two selection methods — leaf clusters are small and dense,
        so their strengths pile up near one, and a cut that drops a fifth of the
        points under ``"eom"`` can drop none under ``"leaf"``.

        ``persistence`` is the across-cluster quantity: cluster stability over
        ``points in the cluster * max lambda in the tree``, so a group that
        survives a wide range of densities scores near one and a group that
        exists only in a narrow band scores near zero. Duplicated points make
        the maximum lambda infinite and the ratio undefined; the convention
        there, matching the reference implementation, is ``1.0``.

        It is *not* a purity signal. Two overlapping populations are one density
        mode and persist as one — that is why excess of mass merges them — so a
        merged mixture can top the ranking: on simulated two-population FRET
        tables the 50 %-pure merge scored 0.45–0.66 against 0.44–0.46 for a pure
        population. To ask whether a cluster is one population or two, look for
        *cluster children* of it in the condensed tree
        (``parent == root`` and ``child >= n_points``): a merged mixture has
        them, a single population has none.
    """
    import numpy as _np
    x = _np.ascontiguousarray(x, dtype=_np.float64)
    if x.ndim != 2:
        raise ValueError("hdbscan: x must be a 2D (n_samples, n_features) table")
    n_points = int(x.shape[0])
    if n_points < 2:
        raise ValueError("hdbscan: at least two points are required")
    if min_samples is None:
        min_samples = min_cluster_size

    mst = _np.asarray(mutual_reachability_mst(x, int(min_samples), float(alpha)))
    parent, child, lam, size = hdbscan_condensed_tree(
        _np.ascontiguousarray(mst[:, 0].astype(_np.int64)),
        _np.ascontiguousarray(mst[:, 1].astype(_np.int64)),
        _np.ascontiguousarray(mst[:, 2]),
        int(min_cluster_size))
    selected = hdbscan_select_clusters(
        parent, child, lam, size, cluster_selection_method,
        bool(allow_single_cluster), float(cluster_selection_epsilon),
        int(max_cluster_size))
    roots = _np.asarray(hdbscan_label_points(parent, child, selected, n_points))
    strength = _np.asarray(hdbscan_membership_strengths(parent, child, lam, roots))

    # A point still rooted at the root cluster was claimed by nothing.
    labels = _np.full(n_points, -1, dtype=_np.int64)
    claimed = roots != n_points
    if claimed.any():
        for label, root in enumerate(_np.unique(roots[claimed])):
            labels[roots == root] = label
    elif allow_single_cluster and selected[n_points]:
        # The root itself is the cluster, and then membership is a cut rather
        # than a tree walk: a point joins it once it is denser than the loosest
        # of the root's own children, or than 1/epsilon if one was given.
        point_lambda = _np.zeros(n_points)
        rows = child < n_points
        point_lambda[child[rows]] = lam[rows]
        threshold = (1.0 / cluster_selection_epsilon
                     if cluster_selection_epsilon != 0.0
                     else lam[parent == n_points].max())
        labels[point_lambda >= threshold] = 0

    strength[labels < 0] = 0.0

    # Stability scales with a cluster's size, so the comparable form divides it
    # out: `stability / (size * max lambda)`, the reference implementation's
    # `cluster_persistence_`.
    n_clusters = int(labels.max()) + 1
    persistence = _np.ones(n_clusters)
    if n_clusters:
        stability = _np.asarray(hdbscan_cluster_stability(parent, child, lam, size))
        max_lambda = float(lam.max())
        roots_by_label = _np.unique(roots[labels >= 0]) if claimed.any() else \
            _np.array([n_points])
        for label in range(n_clusters):
            in_cluster = int((labels == label).sum())
            if _np.isinf(max_lambda) or max_lambda == 0.0 or in_cluster == 0:
                continue
            persistence[label] = (stability[roots_by_label[label]]
                                  / (in_cluster * max_lambda))

    return HdbscanResult(labels, strength, persistence)
%}
#endif

// Restore the global handler rather than clearing it. A bare `%exception;`
// resets to NOTHING -- not to whatever was in force before, which is what the
// old comment here claimed -- so it disarms every interface included after
// this one, and a C++ throw from any of them terminates the interpreter
// instead of raising (BUGS 2026-08-11: a bare clear in Fdc2D.i aborted the
// whole test suite at CLSMSuperRes.temporal_combine). These files are safe
// today only because they happen to precede MicrotimeLinearization.i, which
// reinstalls it; that is include order, not design. Keep this body identical
// to MicrotimeLinearization.i's.
%exception {
    try {
        $action
    } catch (const std::invalid_argument& e) {
        SWIG_exception(SWIG_ValueError, e.what());
    } catch (const std::exception& e) {
        SWIG_exception(SWIG_RuntimeError, e.what());
    } catch (...) {
        SWIG_exception(SWIG_UnknownError, "Unknown exception");
    }
}
