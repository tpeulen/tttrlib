"""Record the HDBSCAN reference fixture for ``test_math_ab_clustering.py``.

Runs under an environment that has the **hdbscan** package (McInnes, Healy &
Astels — the reference implementation of Campello, Moulavi & Sander), and
writes ``test/data/reference/math_ab_clustering_reference.npz``.

    # tttrlib must be the *working tree's* build, so inherit it rather than
    # letting pip resolve an old release that predates these kernels:
    uv venv /tmp/refenv -p "$(which python)" --system-site-packages
    uv pip install --python /tmp/refenv/bin/python hdbscan
    /tmp/refenv/bin/python test/python/misc/gen_math_ab_clustering_reference.py

**Why a recorded fixture rather than a live import.** scikit-learn is the live
reference for selection and membership strengths and covers those thoroughly.
It does not report *cluster persistence* at all — only the standalone `hdbscan`
package does — so persistence has no live reference in the test environment,
and on Apple silicon the conda-forge build of that package is x86_64 and cannot
be imported at all. A recorded fixture is the only honest way to hold
`hdbscan_cluster_stability` to an independent implementation, so that is what
this writes.

What is recorded, and from what:

* the **MST edge list** tttrlib produced for each case, so the reference and
  the test are compared on the same spanning tree and the tie order sklearn
  gets wrong cannot enter (see ``test_math_ab_clustering.py``);
* ``hdbscan._hdbscan_linkage.label`` → ``condense_tree`` → ``compute_stability``
  → ``get_clusters`` on that edge list, giving the reference's **labels**,
  **probabilities** and **cluster_persistence** for both selection methods.

The inputs travel in the file with the outputs, so nothing depends on a random
stream being reproducible across NumPy versions.
"""

import os

import numpy as np

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                   "data", "reference", "math_ab_clustering_reference.npz")

try:
    import tttrlib
except ImportError as exc:  # pragma: no cover - generator only
    raise SystemExit(
        "This generator needs both tttrlib (for the MST) and hdbscan (for the "
        "reference) in one environment. Install tttrlib into the reference "
        "venv, or run it from an environment that has both."
    ) from exc


def canonical_mst(x, min_samples):
    """tttrlib's MST, sorted in the total edge order with `source < target`.

    Any tttrlib recent enough to *return* that order gets it back unchanged;
    older ones returned Boruvka's round order, and this sorts it. Doing it here
    rather than trusting the version means the reference venv can hold whatever
    tttrlib pip resolves -- the spanning tree is unique under the total edge
    order, so canonicalising the rows makes the recorded list identical either
    way, and the test asserts that today's kernel reproduces it.
    """
    mst = np.asarray(tttrlib.mutual_reachability_mst(x, min_samples, 1.0))
    low = np.minimum(mst[:, 0], mst[:, 1])
    high = np.maximum(mst[:, 0], mst[:, 1])
    rows = np.column_stack((low, high, mst[:, 2]))
    return np.ascontiguousarray(rows[np.lexsort((high, low, mst[:, 2]))])


SETTINGS = [(5, 5), (10, 5), (15, 15)]
METHODS = ("eom", "leaf")


def data_sets():
    """Six shapes plus a duplicated-point set.

    Duplicates matter here specifically: a zero merge distance is an infinite
    lambda, the maximum lambda over the tree is then infinite, and persistence
    is undefined. Both sides answer 1.0 by convention, and a fixture that never
    contained duplicates would not be holding anyone to that.
    """
    from sklearn.datasets import make_blobs, make_moons
    rng = np.random.default_rng(0)
    return {
        "blobs3": make_blobs(300, centers=3, cluster_std=0.6, random_state=1)[0],
        "blobs_noise": np.vstack([
            make_blobs(300, centers=3, cluster_std=0.6, random_state=2)[0],
            rng.uniform(-12, 12, (60, 2))]),
        "moons": make_moons(300, noise=0.06, random_state=3)[0],
        "single": rng.normal(size=(200, 2)),
        "noise3d": rng.uniform(size=(200, 3)),
        "bridge": np.vstack([rng.normal(0, 1, (150, 2)), rng.normal(8, 1, (150, 2)),
                             np.c_[np.linspace(0, 8, 20), np.linspace(0, 8, 20)]]),
        # 20 copies of each point, so even at min_samples=15 the k-th
        # neighbour is still a duplicate: the core distance is 0, the
        # merge distance is 0, and lambda is infinite. Four copies (the
        # first attempt here) is not enough and the branch never runs.
        "duplicates": np.repeat(rng.normal(size=(30, 2)), 20, axis=0),
    }


def main():
    from hdbscan._hdbscan_linkage import label
    from hdbscan._hdbscan_tree import condense_tree, compute_stability, get_clusters

    payload = {}
    tags = []
    for name, x in data_sets().items():
        x = np.ascontiguousarray(x, dtype=np.float64)
        for min_cluster_size, min_samples in SETTINGS:
            mst = canonical_mst(x, min_samples)
            for method in METHODS:
                tag = "%s_%d_%d_%s" % (name, min_cluster_size, min_samples, method)
                # `label` turns the MST edge list into the single-linkage tree;
                # feeding condense_tree the edge list directly returns an empty
                # tree and every comparison then passes vacuously.
                tree = condense_tree(label(mst), min_cluster_size)
                labels, probabilities, persistence = get_clusters(
                    tree, compute_stability(tree),
                    cluster_selection_method=method,
                    allow_single_cluster=False,
                    cluster_selection_epsilon=0.0)
                payload["edges_" + tag] = mst
                payload["min_cluster_size_" + tag] = min_cluster_size
                payload["labels_" + tag] = np.asarray(labels)
                payload["probabilities_" + tag] = np.asarray(probabilities)
                payload["persistence_" + tag] = np.asarray(persistence)
                tags.append(tag)

    payload["tags"] = np.array(tags)
    np.savez_compressed(os.path.abspath(OUT), **payload)
    print("wrote %s (%d cases)" % (os.path.abspath(OUT), len(tags)))


if __name__ == "__main__":
    main()
