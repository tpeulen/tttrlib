

def _embedding_points(data):
    import numpy as _np
    x = _np.ascontiguousarray(data, dtype=_np.float64)
    if x.ndim == 1:
        x = x[:, None]
    if x.ndim != 2 or x.shape[0] < 2:
        raise ValueError("data must be a 2-D array (n_samples, n_features) with at least two rows")
    return x


def tsne(data, n_components=2, perplexity=30.0, early_exaggeration=12.0,
         learning_rate="auto", max_iter=1000, n_iter_without_progress=300,
         min_grad_norm=1e-7, init="pca", method="barnes_hut", angle=0.5,
         random_state=0, return_info=False):
    """t-SNE embedding of the rows of ``data`` (scikit-learn's ``TSNE``).

    Parameters follow ``sklearn.manifold.TSNE``: ``init`` is ``"pca"``,
    ``"random"`` or an ``(n_samples, n_components)`` array; ``method`` is
    ``"barnes_hut"`` (n_components 2 or 3) or ``"exact"``; ``random_state``
    seeds the random initialisation only (PCA and the optimisation are
    deterministic).

    Returns the ``(n_samples, n_components)`` embedding, or with
    ``return_info=True`` a tuple ``(embedding, kl_divergence, n_iter)``.
    """
    import numpy as _np
    x = _embedding_points(data)
    n = x.shape[0]
    if isinstance(init, str):
        if init == "pca":
            y0 = _np.empty((0, n_components))
        elif init == "random":
            rng = _np.random.default_rng(random_state)
            y0 = rng.standard_normal((n, n_components)) * 1e-4
        else:
            raise ValueError("init must be 'pca', 'random' or an array")
    else:
        y0 = _np.ascontiguousarray(init, dtype=_np.float64)
    lr = -1.0 if (isinstance(learning_rate, str) and learning_rate == "auto") else float(learning_rate)
    m = {"exact": 0, "barnes_hut": 1}.get(method)
    if m is None:
        raise ValueError("method must be 'barnes_hut' or 'exact'")
    y, kl, it = tsne_embed(x, y0, int(n_components), float(perplexity), float(early_exaggeration),
                           lr, int(max_iter), int(n_iter_without_progress), float(min_grad_norm),
                           m, float(angle))
    y = _np.asarray(y)
    return (y, kl, it) if return_info else y


def umap(data, n_components=2, n_neighbors=15, min_dist=0.1, spread=1.0,
         n_epochs=None, learning_rate=1.0, negative_sample_rate=5,
         repulsion_strength=1.0, local_connectivity=1.0, set_op_mix_ratio=1.0,
         init="spectral", a=None, b=None, random_state=0):
    """UMAP embedding of the rows of ``data`` (umap-learn's ``UMAP``).

    Parameters follow ``umap.UMAP`` (Euclidean metric): ``init`` is
    ``"spectral"``, ``"random"`` or an ``(n_samples, n_components)`` array;
    ``a``/``b`` default to the fit from ``min_dist`` and ``spread``;
    ``random_state`` (an int >= 0) seeds the jitter and negative sampling, so
    a run is reproducible.

    Returns the ``(n_samples, n_components)`` embedding.
    """
    import numpy as _np
    x = _embedding_points(data)
    n = x.shape[0]
    if isinstance(init, str):
        if init == "spectral":
            y0 = _np.empty((0, n_components))
        elif init == "random":
            rng = _np.random.default_rng(random_state)
            y0 = rng.uniform(-10.0, 10.0, (n, n_components))
        else:
            raise ValueError("init must be 'spectral', 'random' or an array")
    else:
        y0 = _np.ascontiguousarray(init, dtype=_np.float64)
    seed = 0 if random_state is None else int(random_state)
    y = umap_embed(x, y0, int(n_components), int(min(n_neighbors, n)), float(min_dist),
                   float(spread), 0 if n_epochs is None else int(n_epochs),
                   float(learning_rate), float(negative_sample_rate),
                   float(repulsion_strength), float(local_connectivity),
                   float(set_op_mix_ratio), -1.0 if a is None else float(a),
                   -1.0 if b is None else float(b), seed)
    return _np.asarray(y)


def image_features_to_points(features, mask=None, standardize=True):
    """Per-pixel feature vectors of an image, as rows to embed.

    ``features`` is an ``(H, W, n_features)`` array (channels last) or a
    sequence of ``(H, W)`` images -- intensity, mean lifetime, phasor g and s,
    ... . Pixels outside ``mask`` (an ``(H, W)`` boolean array) and pixels with
    any non-finite feature are left out. With ``standardize`` every feature is
    scaled to zero mean and unit variance, so a lifetime in ns and a count in
    the thousands weigh alike in the distances.

    Returns ``(points, index)``: the ``(n_pixels, n_features)`` points and the
    flat pixel index of each, for ``points_to_label_image``.
    """
    import numpy as _np
    if isinstance(features, (list, tuple)):
        f = _np.stack([_np.asarray(c, dtype=_np.float64) for c in features], axis=-1)
    else:
        f = _np.asarray(features, dtype=_np.float64)
        if f.ndim == 2:
            f = f[..., None]
    if f.ndim != 3:
        raise ValueError("features must be (H, W, n_features) or a sequence of (H, W) images")
    h, w, k = f.shape
    flat = f.reshape(h * w, k)
    keep = _np.all(_np.isfinite(flat), axis=1)
    if mask is not None:
        m = _np.asarray(mask, dtype=bool)
        if m.shape != (h, w):
            raise ValueError("mask must have the image's (H, W) shape")
        keep &= m.ravel()
    index = _np.flatnonzero(keep)
    points = _np.ascontiguousarray(flat[index])
    if standardize and len(points):
        sd = points.std(axis=0)
        sd[sd == 0] = 1.0
        points = (points - points.mean(axis=0)) / sd
    return points, index


def points_to_label_image(labels, index, shape, fill=-1):
    """Cluster labels of embedded pixels back into an ``shape`` label image;
    pixels that were not embedded get ``fill``."""
    import numpy as _np
    out = _np.full(int(_np.prod(shape)), fill, dtype=_np.int64)
    out[_np.asarray(index, dtype=_np.int64)] = _np.asarray(labels, dtype=_np.int64)
    return out.reshape(shape)
