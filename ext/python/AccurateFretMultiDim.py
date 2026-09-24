# SPDX-License-Identifier: BSD-3-Clause
# Python face of AccurateFretMultiDim.h. Appended to the SWIG module after
# AccurateFret.py and AccurateFretCalibrate.py, whose helpers it uses.


def _afret_nd_dict(r, n_rows):
    np = _afret_np()
    k, d = int(r.n_components), int(r.n_dims)
    return {
        "n_components": k,
        "weights": np.asarray(r.weights, dtype=float),
        "means": np.asarray(r.means, dtype=float).reshape(k, d),
        "sigmas": np.asarray(r.sigmas, dtype=float).reshape(k, d),
        "responsibilities": np.asarray(r.responsibilities, dtype=float).reshape(n_rows, k),
        "labels": np.asarray(r.labels, dtype=int),
        "log_likelihood": float(r.log_likelihood),
        "bic": float(r.bic),
        "n_iter": int(r.n_iter),
        "bic_by_k": {int(a): float(b) for a, b in zip(r.bic_k, r.bic_values)},
    }


def _afret_matrix(x):
    np = _afret_np()
    x = np.asarray(x, dtype=float)
    if x.ndim == 1:
        x = x[:, None]
    return x, int(x.shape[0]), int(x.shape[1])


def gaussian_mixture_nd(x, n_components, *, n_iterations=300, tolerance=1e-7, sigma_floor=1e-3):
    """Diagonal-covariance Gaussian mixture over the columns of ``x`` (n_rows, n_dims).

    Columns are standardised internally; NaNs are missing values and are
    marginalised. Components are sorted by their first-column mean.

    Returns
    -------
    dict
        ``{"n_components", "weights", "means" (k, d), "sigmas" (k, d),
        "responsibilities" (n_rows, k), "labels" (-1 for all-NaN rows),
        "log_likelihood", "bic", "n_iter", "bic_by_k"}``.
    """
    m, n, d = _afret_matrix(x)
    r = _afret_gaussian_mixture_nd(_afret_vec(m), n, d, int(n_components), int(n_iterations),
                                   float(tolerance), float(sigma_floor))
    return _afret_nd_dict(r, n)


def best_gaussian_mixture_nd(x, *, max_components=6, min_weight=0.02):
    """BIC-selected :func:`gaussian_mixture_nd` over 1 .. ``max_components``."""
    m, n, d = _afret_matrix(x)
    return _afret_nd_dict(_afret_best_gaussian_mixture_nd(_afret_vec(m), n, d, int(max_components),
                                                          float(min_weight)), n)


def classify_populations_nd(columns, names=None, *, donor_only_above=0.75,
                            acceptor_only_below=0.25, max_components=6, min_population=20,
                            min_probability=0.9):
    """Donor-only, acceptor-only and FRET bursts from every declared dimension.

    Parameters
    ----------
    columns : mapping or array_like
        ``{name: per-burst array}`` (``names`` then selects and orders them) or
        an ``(n_bursts, len(names))`` array. Names from the declared vocabulary
        (``AFRET_DIMENSIONS``: ``S``, ``E``, ``tau_d``, ``tau_a``, ``r_d``,
        ``r_a``) are interpreted; others only help to separate components.

    Returns
    -------
    dict
        As :func:`classify_es_populations`, with ``"fret_probabilities"``
        ``(n_bursts, n_fret_populations)`` and the component table under
        ``"components"``.
    """
    np = _afret_np()
    if hasattr(columns, "keys"):
        names = list(names or columns.keys())
        m = np.stack([np.asarray(columns[k], dtype=float).ravel() for k in names], axis=1)
    else:
        m, _, _ = _afret_matrix(columns)
        names = list(names)
    r = _afret_classify_populations_nd(_afret_vec(m), int(m.shape[0]), VectorString(names),
                                       float(donor_only_above), float(acceptor_only_below),
                                       int(max_components), int(min_population),
                                       float(min_probability))
    return _afret_split_dict(r)


def _afret_named_matrix(columns, names):
    np = _afret_np()
    if hasattr(columns, "keys"):
        names = list(names or columns.keys())
        m = np.stack([np.asarray(columns[k], dtype=float).ravel() for k in names], axis=1)
    else:
        m, _, _ = _afret_matrix(columns)
        names = list(names)
    return m, names


def classify_populations_hdbscan(columns, names=None, *, donor_only_above=0.75,
                                 acceptor_only_below=0.25, min_population=20,
                                 min_probability=0.9, min_cluster_fraction=0.02,
                                 min_cluster_size=0, min_samples=0, max_points=10000,
                                 selection="leaf", epsilon=0.0, width_floor=None):
    """Donor-only, acceptor-only and FRET bursts by density-based clustering (HDBSCAN).

    Columns as in :func:`classify_populations_nd`. Each column is centred on
    its median and scaled by its robust width (IQR / 1.349); missing values
    sit on a sentinel below the column. At most ``max_points`` bursts are
    clustered (an even stride), the rest join a cluster through their nearest
    clustered neighbours. ``min_cluster_size``/``min_samples`` of 0 mean
    ``max(10, min_cluster_fraction * clustered bursts)``. Assignment
    probabilities come from per-cluster Gaussians; ``width_floor`` (the
    shape of the columns, NaN where unknown) is each burst's shot-noise width,
    the least width of a species in the same-species merge. Assignment
    probabilities come from per-cluster Gaussians (HDBSCAN's membership strength
    is a rank within one cluster, returned as ``"membership"``).

    Returns
    -------
    dict
        As :func:`classify_populations_nd`, plus ``"noise"`` (bursts HDBSCAN
        left unclaimed: in no class), ``"cluster_labels"`` and ``"membership"``.
    """
    m, names = _afret_named_matrix(columns, names)
    r = _afret_classify_populations_hdbscan(
        _afret_vec(m), int(m.shape[0]), VectorString(names), float(donor_only_above),
        float(acceptor_only_below), int(min_population), float(min_probability),
        float(min_cluster_fraction), int(min_cluster_size), int(min_samples), int(max_points),
        str(selection), float(epsilon),
        [] if width_floor is None else _afret_vec(_afret_np().asarray(width_floor, dtype=float).reshape(m.shape)))
    return _afret_split_dict(r)


def flag_dimension_outliers(columns, names=None, *, es_lo=-0.2, es_hi=1.2, tau_max=20.0,
                            r_lo=-0.5, r_hi=1.0, fence=1.0, quantile=0.025):
    """Per-dimension outliers of the gating columns, before any scaling.

    Physical range first (S/E in ``[es_lo, es_hi]``, lifetimes in ``(0, tau_max]``,
    anisotropies in ``[r_lo, r_hi]``, +-inf always), then, except for S and E,
    the fence ``[q_lo - fence w, q_hi + fence w]`` (``q_lo``/``q_hi`` the
    ``quantile``/``1 - quantile`` quantiles of what passed, ``w = q_hi - q_lo``).
    NaN is a missing value, not an outlier.

    Returns
    -------
    dict
        ``{"n", "mask", "by_dimension": {dim: {"range", "fence", "lo", "hi"}}}``.
    """
    np = _afret_np()
    m, names = _afret_named_matrix(columns, names)
    r = _afret_flag_dimension_outliers(_afret_vec(m), int(m.shape[0]), VectorString(names),
                                       float(es_lo), float(es_hi), float(tau_max), float(r_lo),
                                       float(r_hi), float(fence), float(quantile))
    mask = np.asarray(r.outlier, dtype=bool)
    by = {str(nm): {"range": int(a), "fence": int(b), "lo": float(lo), "hi": float(hi)}
          for nm, a, b, lo, hi in zip(r.dimensions, r.n_range, r.n_fence, r.lo, r.hi)}
    return {"n": int(mask.sum()), "mask": mask, "by_dimension": by}
