"""Record the t-SNE / UMAP reference fixture for ``test_embedding.py``.

Runs under an environment with **scikit-learn** and **umap-learn** and writes
``test/data/reference/embedding_reference.npz``. tttrlib is not needed:

    uv venv /tmp/refenv --python 3.12
    uv pip install --python /tmp/refenv/bin/python umap-learn scikit-learn
    /tmp/refenv/bin/python test/python/misc/gen_embedding_reference.py

**Why recorded.** Neither reference is a test dependency (CI has neither, and
umap-learn needs numba), so the deterministic parts are recorded once and the
test holds tttrlib to them offline.

What is recorded:

* the **data sets** themselves (rounded to float32-exact values, because
  umap-learn computes in float32), so nothing depends on a random stream;
* scikit-learn's **joint probabilities** P, exact (``_joint_probabilities``) and
  over the nearest neighbours (``_joint_probabilities_nn``), at two perplexities;
* umap-learn's **a, b** (``find_ab_params``) for several (spread, min_dist);
* umap-learn's **fuzzy simplicial set** over *exact* neighbours (passed in, so
  NNDescent's approximation cannot enter), for two neighbour counts;
* umap-learn's **spectral initialisation** (``spectral_layout``) of the pruned
  graphs, which ``umap_spectral_layout`` must reproduce column for column;
* a **segmentation** case (a synthetic FLIM image) and the ARI umap-learn +
  HDBSCAN reach on it for eight seeds -- the outcome is bimodal per seed for
  every implementation, so the claim tested is about the distribution;
* reference **embeddings**: t-SNE exact and Barnes-Hut from one fixed
  initialisation, with their final KL divergence, and UMAP from its own
  spectral initialisation; plus scikit-learn's **trustworthiness** of each, and
  of a deliberately scrambled embedding, to check tttrlib's own score exactly.
"""

import os

import numpy as np
from scipy.spatial.distance import squareform
import sklearn
import umap
from sklearn.manifold import TSNE, trustworthiness
from sklearn.manifold._t_sne import _joint_probabilities, _joint_probabilities_nn
from sklearn.metrics import pairwise_distances
from sklearn.neighbors import NearestNeighbors
from sklearn.cluster import HDBSCAN
from umap.spectral import spectral_layout
from umap.umap_ import find_ab_params, fuzzy_simplicial_set

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "data", "reference", "embedding_reference.npz")


def data_sets():
    """Three Gaussian blobs with background, and two interleaved half-moons
    lifted into 4-D -- clusters a single projection does not separate."""
    rng = np.random.default_rng(20260924)
    centres = np.array([[0, 0, 0, 0, 0], [4, 1, 0, 2, 0], [1, 5, 3, 0, 1]], float)
    blobs = np.vstack([c + rng.normal(0, 0.7, (80, 5)) for c in centres]
                      + [rng.uniform(-2, 7, (30, 5))])
    blob_labels = np.r_[np.repeat([0, 1, 2], 80), np.full(30, -1)]
    t = rng.uniform(0, np.pi, 150)
    m1 = np.c_[np.cos(t), np.sin(t)]
    m2 = np.c_[1 - np.cos(t), 0.5 - np.sin(t)]
    moons2 = np.vstack([m1, m2]) + rng.normal(0, 0.06, (300, 2))
    lift = rng.normal(0, 1, (2, 4))
    moons = moons2 @ lift + rng.normal(0, 0.02, (300, 4))
    moon_labels = np.r_[np.zeros(150, int), np.ones(150, int)]
    as32 = lambda x: np.asarray(x, np.float32).astype(np.float64)
    return {"blobs": (as32(blobs), blob_labels), "moons": (as32(moons), moon_labels)}


def segmentation_image():
    """A synthetic FLIM image whose two regions lie on two parallel bands in
    the (intensity, lifetime) plane: each feature alone overlaps completely,
    the pair separates them."""
    rng = np.random.default_rng(11)
    h, w = 40, 50
    truth = np.zeros((h, w), int)
    truth[10:30, 15:40] = 1
    u = rng.normal(0, 2.0, (h, w))                                    # along the bands
    v = np.where(truth == 1, 0.6, -0.6) + rng.normal(0, 0.1, (h, w))  # across
    intensity = 1000.0 + 150.0 * (u + v)
    lifetime = 3.0 + 0.3 * (u - v)
    mask = np.ones((h, w), bool)
    mask[0, :] = False
    return intensity, lifetime, mask, truth


def standardized_points(intensity, lifetime, mask):
    """What tttrlib.image_features_to_points(standardize=True) gives."""
    f = np.stack([intensity, lifetime], -1).reshape(-1, 2)[mask.ravel()]
    return (f - f.mean(0)) / f.std(0)


def adjusted_rand(a, b):
    from sklearn.metrics import adjusted_rand_score
    return adjusted_rand_score(a, b)


def pruned_spectral(x, k, n_epochs=500):
    """umap-learn's spectral initialisation of x's graph, after the pruning
    simplicial_set_embedding applies (and exact neighbours)."""
    dist, idx = NearestNeighbors(n_neighbors=k).fit(x).kneighbors(x)
    g, _, _ = fuzzy_simplicial_set(x, k, 0, "euclidean", knn_indices=idx, knn_dists=dist)
    g = g.tocoo()
    g.sum_duplicates()
    g.data[g.data < g.data.max() / float(n_epochs)] = 0.0
    g.eliminate_zeros()
    return spectral_layout(x, g, 2, np.random.RandomState(0))


def main():
    rec = {"sklearn_version": sklearn.__version__, "umap_version": umap.__version__}
    rec["ab_cases"] = np.array([[1.0, 0.1], [1.0, 0.0], [1.0, 0.5], [2.0, 0.3], [0.5, 0.05]])
    rec["ab"] = np.array([find_ab_params(s, m) for s, m in rec["ab_cases"]])
    for name, (x, labels) in data_sets().items():
        n = len(x)
        rec[f"{name}_x"] = x
        rec[f"{name}_labels"] = labels
        # --- t-SNE joint probabilities -------------------------------------
        d2 = pairwise_distances(x, squared=True)
        for perp in (10, 30):
            rec[f"{name}_P_exact_{perp}"] = squareform(_joint_probabilities(d2, perp, 0))
            k = min(n - 1, int(3.0 * perp + 1))
            nn = NearestNeighbors(n_neighbors=k, metric="euclidean").fit(x)
            dnn = nn.kneighbors_graph(mode="distance")
            dnn.data **= 2
            P = _joint_probabilities_nn(dnn, perp, 0).tocoo()
            order = np.lexsort((P.col, P.row))
            rec[f"{name}_Pnn_{perp}_row"] = P.row[order].astype(np.int64)
            rec[f"{name}_Pnn_{perp}_col"] = P.col[order].astype(np.int64)
            rec[f"{name}_Pnn_{perp}_val"] = P.data[order].astype(np.float64)
        # --- UMAP fuzzy simplicial set over exact neighbours -----------------
        for k in (10, 15):
            nn = NearestNeighbors(n_neighbors=k).fit(x)
            dist, idx = nn.kneighbors(x)          # the point itself first
            g, _, _ = fuzzy_simplicial_set(x, k, 0, "euclidean",
                                           knn_indices=idx, knn_dists=dist)
            g = g.tocoo()
            order = np.lexsort((g.col, g.row))
            rec[f"{name}_G{k}_row"] = g.row[order].astype(np.int64)
            rec[f"{name}_G{k}_col"] = g.col[order].astype(np.int64)
            rec[f"{name}_G{k}_val"] = g.data[order].astype(np.float64)
        # --- reference embeddings -------------------------------------------
        y0 = np.random.default_rng(7).standard_normal((n, 2)) * 1e-4
        rec[f"{name}_tsne_init"] = y0
        for method in ("exact", "barnes_hut"):
            ts = TSNE(n_components=2, perplexity=30, init=y0.copy(), method=method,
                      random_state=0, max_iter=1000)
            y = ts.fit_transform(x)
            rec[f"{name}_tsne_{method}"] = y.astype(np.float64)
            rec[f"{name}_tsne_{method}_kl"] = float(ts.kl_divergence_)
            rec[f"{name}_tsne_{method}_trust"] = trustworthiness(x, y, n_neighbors=10)
        u = umap.UMAP(n_neighbors=15, min_dist=0.1, random_state=0).fit_transform(x)
        rec[f"{name}_umap"] = u.astype(np.float64)
        rec[f"{name}_umap_trust"] = trustworthiness(x, u, n_neighbors=10)
        scr = np.random.default_rng(3).permutation(x[:, :2])
        rec[f"{name}_scrambled"] = scr
        rec[f"{name}_scrambled_trust"] = trustworthiness(x, scr, n_neighbors=10)
        rec[f"{name}_spectral"] = pruned_spectral(x, 15)
    # --- segmentation: umap-learn + HDBSCAN over seeds ------------------------
    intensity, lifetime, mask, truth = segmentation_image()
    rec["seg_intensity"], rec["seg_lifetime"] = intensity, lifetime
    rec["seg_mask"], rec["seg_truth"] = mask, truth
    pts = standardized_points(intensity, lifetime, mask)
    rec["seg_spectral"] = pruned_spectral(pts, 30)
    rec["seg_umap_ari"] = np.array([
        adjusted_rand(HDBSCAN(min_cluster_size=100).fit_predict(
            umap.UMAP(n_neighbors=30, min_dist=0.0, random_state=s).fit_transform(pts)),
            truth[mask]) for s in range(8)])
    print("umap-learn segmentation ARI per seed:", np.round(rec["seg_umap_ari"], 3))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    np.savez_compressed(OUT, **rec)
    print("wrote", os.path.relpath(OUT), "-", len(rec), "arrays")


if __name__ == "__main__":
    main()
