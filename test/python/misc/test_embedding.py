"""t-SNE and UMAP (Embedding.h) against scikit-learn and umap-learn.

The references are recorded in ``test/data/reference/embedding_reference.npz``
by ``gen_embedding_reference.py`` (scikit-learn 1.9, umap-learn 0.5), so this
runs without either installed.

Two kinds of claim, kept apart:

* **Same answer.** Where a reference has a single right answer -- t-SNE's joint
  probabilities P, exact and over the nearest neighbours; UMAP's a, b and its
  fuzzy simplicial set; the trustworthiness score -- tttrlib must reproduce it
  to rounding. These are the parts every embedding is built from, so a
  mistake anywhere in them shows here, not as a vaguely worse picture.
* **As good an answer.** An embedding is the end of a non-convex optimisation
  with a random stream in it, so no two implementations agree point for point.
  There the claim is that tttrlib's embedding is as good as the reference's by
  the measures that matter for segmentation: KL divergence (t-SNE's own
  objective), trustworthiness, and whether the known clusters come apart.
"""
import os

import numpy as np
import pytest

import tttrlib

REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "data", "reference", "embedding_reference.npz")
SETS = ("blobs", "moons")


@pytest.fixture(scope="module")
def ref():
    if not os.path.exists(REF):
        pytest.skip("embedding reference fixture not recorded")
    return np.load(REF)


def adjusted_rand(a, b):
    """Adjusted Rand index (Hubert & Arabie), no scikit-learn needed."""
    a, b = np.asarray(a), np.asarray(b)
    _, ai = np.unique(a, return_inverse=True)
    _, bi = np.unique(b, return_inverse=True)
    table = np.zeros((ai.max() + 1, bi.max() + 1))
    np.add.at(table, (ai, bi), 1)
    comb = lambda x: x * (x - 1) / 2.0
    s = comb(table).sum()
    sa, sb = comb(table.sum(1)).sum(), comb(table.sum(0)).sum()
    expected = sa * sb / comb(len(a))
    return (s - expected) / (0.5 * (sa + sb) - expected)


def recovered(y, labels, min_cluster_size=20):
    """ARI of HDBSCAN on the embedding against the known clusters, scored on
    the points that belong to one (background points carry -1)."""
    found = np.asarray(tttrlib.hdbscan(y, min_cluster_size=min_cluster_size).labels)
    member = labels >= 0
    return adjusted_rand(found[member], labels[member])


# ------------------------------------------------------------ same answer --

@pytest.mark.parametrize("name", SETS)
@pytest.mark.parametrize("perplexity", (10, 30))
def test_exact_joint_probabilities_match_sklearn(ref, name, perplexity):
    ours = np.asarray(tttrlib.tsne_joint_probabilities(ref[f"{name}_x"], float(perplexity)))
    theirs = ref[f"{name}_P_exact_{perplexity}"]
    assert ours.shape == theirs.shape
    np.testing.assert_allclose(ours, theirs, rtol=1e-5, atol=1e-12)
    # sums to one up to the machine-epsilon floor on every tiny entry (n^2 of
    # them), exactly as sklearn's P does
    assert ours.sum() == pytest.approx(theirs.sum(), abs=1e-12)
    assert ours.sum() == pytest.approx(1.0, abs=len(ours) ** 2 * 1e-15)


@pytest.mark.parametrize("name", SETS)
@pytest.mark.parametrize("perplexity", (10, 30))
def test_neighbour_joint_probabilities_match_sklearn(ref, name, perplexity):
    row, col, val = tttrlib.tsne_joint_probabilities_nn(ref[f"{name}_x"], float(perplexity))
    np.testing.assert_array_equal(row, ref[f"{name}_Pnn_{perplexity}_row"])
    np.testing.assert_array_equal(col, ref[f"{name}_Pnn_{perplexity}_col"])
    np.testing.assert_allclose(val, ref[f"{name}_Pnn_{perplexity}_val"], rtol=1e-5, atol=1e-12)


def test_ab_match_umap_learn(ref):
    for (spread, min_dist), (a, b) in zip(ref["ab_cases"], ref["ab"]):
        oa, ob = tttrlib.umap_find_ab_params(float(spread), float(min_dist))
        assert oa == pytest.approx(a, rel=1e-4), (spread, min_dist)
        assert ob == pytest.approx(b, rel=1e-4), (spread, min_dist)


@pytest.mark.parametrize("name", SETS)
@pytest.mark.parametrize("k", (10, 15))
def test_fuzzy_simplicial_set_matches_umap_learn(ref, name, k):
    row, col, val = tttrlib.umap_fuzzy_graph(ref[f"{name}_x"], k, 1.0, 1.0)
    np.testing.assert_array_equal(row, ref[f"{name}_G{k}_row"])
    np.testing.assert_array_equal(col, ref[f"{name}_G{k}_col"])
    np.testing.assert_allclose(val, ref[f"{name}_G{k}_val"], rtol=0, atol=1e-6)


def pruned_graph(x, k, n_epochs=500):
    row, col, val = tttrlib.umap_fuzzy_graph(x, k, 1.0, 1.0)
    keep = ~(val < val.max() / n_epochs)
    return row[keep], col[keep], val[keep]


@pytest.mark.parametrize("case", ("blobs", "moons", "seg"))
def test_spectral_layout_matches_umap_learn(ref, case):
    """Eigenvectors 2 and 3 of the normalised Laplacian, column for column (up
    to sign). The seg graph is the hard one: its leading eigenvalues sit ~1e-4
    apart, which a restart that loses the Krylov structure never resolves."""
    if case == "seg":
        x, _ = segmentation_points(ref)
        k = 30
    else:
        x, k = ref[f"{case}_x"], 15
    ours = np.asarray(tttrlib.umap_spectral_layout(*pruned_graph(x, k), len(x), 2, 0))
    theirs = ref[f"{case}_spectral"]
    for j in range(2):
        cos = abs(ours[:, j] @ theirs[:, j]) / np.linalg.norm(ours[:, j]) / np.linalg.norm(theirs[:, j])
        assert cos > 0.9999, (case, j, cos)


@pytest.mark.parametrize("name", SETS)
@pytest.mark.parametrize("which", ("tsne_exact", "tsne_barnes_hut", "umap", "scrambled"))
def test_trustworthiness_matches_sklearn(ref, name, which):
    y = ref[f"{name}_{which}"]
    t = tttrlib.embedding_trustworthiness(ref[f"{name}_x"], np.ascontiguousarray(y), 10)
    assert t == pytest.approx(float(ref[f"{name}_{which}_trust"]), abs=1e-12)


# ------------------------------------------------------ as good an answer --

@pytest.mark.parametrize("name", SETS)
@pytest.mark.parametrize("method", ("exact", "barnes_hut"))
def test_tsne_is_as_good_as_sklearn(ref, name, method):
    x = ref[f"{name}_x"]
    y, kl, it = tttrlib.tsne(x, perplexity=30, init=ref[f"{name}_tsne_init"],
                             method=method, return_info=True)
    ref_kl = float(ref[f"{name}_tsne_{method}_kl"])
    assert kl <= ref_kl * 1.05 + 1e-3, (kl, ref_kl)
    t = tttrlib.embedding_trustworthiness(x, y, 10)
    assert t >= float(ref[f"{name}_tsne_{method}_trust"]) - 0.01
    labels = ref[f"{name}_labels"]
    assert recovered(y, labels) >= recovered(ref[f"{name}_tsne_{method}"], labels) - 0.05


@pytest.mark.parametrize("name", SETS)
def test_umap_is_as_good_as_umap_learn(ref, name):
    x = ref[f"{name}_x"]
    y = tttrlib.umap(x, n_neighbors=15, min_dist=0.1, random_state=0)
    assert y.shape == (len(x), 2) and np.all(np.isfinite(y))
    t = tttrlib.embedding_trustworthiness(x, y, 10)
    assert t >= float(ref[f"{name}_umap_trust"]) - 0.02
    labels = ref[f"{name}_labels"]
    assert recovered(y, labels) >= recovered(ref[f"{name}_umap"], labels) - 0.05


# ------------------------------------------------------------- behaviour --

def test_umap_is_reproducible_for_a_seed_and_not_across_seeds(ref):
    x = ref["blobs_x"]
    a = tttrlib.umap(x, random_state=3, n_epochs=50)
    b = tttrlib.umap(x, random_state=3, n_epochs=50)
    c = tttrlib.umap(x, random_state=4, n_epochs=50)
    np.testing.assert_array_equal(a, b)
    assert not np.array_equal(a, c)


def test_tsne_pca_init_is_deterministic(ref):
    x = ref["moons_x"]
    a = tttrlib.tsne(x, max_iter=250, method="exact")
    b = tttrlib.tsne(x, max_iter=250, method="exact")
    np.testing.assert_array_equal(a, b)


def test_three_components(ref):
    x = ref["blobs_x"]
    assert tttrlib.tsne(x, n_components=3, max_iter=300).shape == (len(x), 3)
    assert tttrlib.umap(x, n_components=3, n_epochs=50).shape == (len(x), 3)


def test_bad_input_raises_value_error():
    with pytest.raises(ValueError):
        tttrlib.tsne(np.array([[1.0, 2.0]]))                  # one row
    with pytest.raises(ValueError):
        tttrlib.tsne(np.random.default_rng(0).normal(size=(50, 3)), perplexity=60)
    with pytest.raises(ValueError):
        tttrlib.tsne(np.random.default_rng(0).normal(size=(50, 5)), n_components=4)
    with pytest.raises(ValueError):
        x = np.random.default_rng(0).normal(size=(40, 3))
        x[3, 1] = np.nan
        tttrlib.umap(x)


def best_single_threshold_ari(feature, truth):
    """The best any threshold on one feature alone can do."""
    return max(adjusted_rand(feature > t, truth) for t in np.quantile(feature, np.linspace(0.02, 0.98, 49)))


def segmentation_points(ref):
    intensity, lifetime = ref["seg_intensity"], ref["seg_lifetime"]
    points, index = tttrlib.image_features_to_points([intensity, lifetime], mask=ref["seg_mask"])
    return points, index


def test_image_helpers_round_trip(ref):
    intensity, lifetime, mask = ref["seg_intensity"], ref["seg_lifetime"], ref["seg_mask"]
    points, index = segmentation_points(ref)
    assert points.shape == (mask.sum(), 2)
    np.testing.assert_array_equal(index, np.flatnonzero(mask))
    assert np.allclose(points.mean(0), 0, atol=1e-12) and np.allclose(points.std(0), 1)
    img = tttrlib.points_to_label_image(np.arange(len(index)), index, mask.shape)
    assert np.all(img[~mask] == -1)
    np.testing.assert_array_equal(img[mask], np.arange(len(index)))
    lifetime2 = lifetime.copy()
    lifetime2[5, 5] = np.nan                                  # non-finite pixels are left out
    p2, i2 = tttrlib.image_features_to_points([intensity, lifetime2], mask=mask)
    assert len(p2) == len(points) - 1 and 5 * mask.shape[1] + 5 not in i2


@pytest.mark.heavy
def test_segmenting_an_image_is_as_good_as_umap_learn(ref):
    """Embed the per-pixel features, cluster the embedding, map the labels
    back. On this image UMAP + HDBSCAN is bimodal per seed -- umap-learn
    itself reaches ARI ~0.9-0.99 on five seeds of eight and ~0.4 on three --
    so the claim is about the distribution over the same eight seeds: as
    many good segmentations, and as high a median, as umap-learn. And no
    threshold on either feature alone comes close."""
    truth, mask = ref["seg_truth"], ref["seg_mask"]
    assert best_single_threshold_ari(ref["seg_intensity"][mask], truth[mask]) < 0.3
    assert best_single_threshold_ari(ref["seg_lifetime"][mask], truth[mask]) < 0.3
    points, index = segmentation_points(ref)
    aris = []
    for seed in range(8):
        y = tttrlib.umap(points, n_neighbors=30, min_dist=0.0, random_state=seed)
        found = np.asarray(tttrlib.hdbscan(y, min_cluster_size=100).labels)
        seg = tttrlib.points_to_label_image(found, index, mask.shape)
        aris.append(adjusted_rand(seg[mask], truth[mask]))
    aris, theirs = np.array(aris), ref["seg_umap_ari"]
    assert np.sum(aris > 0.9) >= np.sum(theirs > 0.9), (np.round(aris, 3), np.round(theirs, 3))
    assert np.median(aris) >= np.median(theirs) - 0.02, (np.round(aris, 3), np.round(theirs, 3))
