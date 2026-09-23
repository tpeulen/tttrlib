"""HDBSCAN downstream of the MST: condensation and labelling (`Cluster.h`).

`core_distances` and `mutual_reachability_mst` are the first half. These are the
rest — union-find single linkage, dendrogram condensation, and reading a root
off per point — which is 43% of a compiled run and none of which an array
language expresses: union-find, a breadth-first tree walk and a dynamic
compaction are pointer-chasing.

**Three calls, not one, and the boundaries are deliberate.** Cluster
*selection* sits between condensation and labelling and is policy —
excess-of-mass versus leaf selection, `allow_single_cluster`,
`cluster_selection_epsilon` — so it is its own call, substitutable by a caller
with its own rule. It is not, however, *absent*: the two published policies are
`hdbscan_select_clusters`, and the tests below are what says they compose with
the other two into a clustering rather than into three-quarters of one.

Nothing here imports the implementation being replaced. A test that reaches into
a sibling project becomes a skip the day that project moves, and a skip reads
like a pass — so the checks are either structural invariants of the condensed
tree or a recovery test against data whose answer is known in advance.
"""

import unittest

import numpy as np

import tttrlib


def three_blobs(seed=4):
    """Well-separated blobs, with the true membership known before clustering."""
    rng = np.random.default_rng(seed)
    x = np.vstack([rng.normal(0, 1, (120, 2)),
                   rng.normal(8, 1, (120, 2)),
                   rng.normal([0, 8], 1, (60, 2))])
    truth = np.repeat([0, 1, 2], [120, 120, 60])
    return np.ascontiguousarray(x), truth


def sorted_mst(x, k=5):
    """The MST edge list as three arrays. It comes back sorted — the columns
    are only being split out and retyped here."""
    mst = np.asarray(tttrlib.mutual_reachability_mst(x, k, 1.0))
    return (np.ascontiguousarray(mst[:, 0].astype(np.int64)),
            np.ascontiguousarray(mst[:, 1].astype(np.int64)),
            np.ascontiguousarray(mst[:, 2].astype(np.float64)))


def leaf_clusters(parent, child, n_points):
    """Clusters with no *cluster* child.

    Note what this is not: "a cluster that never appears as a parent". Every
    cluster is the parent of the individual points it sheds, so that definition
    finds nothing — it returned an empty set on the first attempt here and made
    a labelling test pass vacuously with everything collapsed to the root.
    """
    has_cluster_child = np.unique(parent[child >= n_points])
    clusters = np.unique(np.concatenate([[n_points], child[child >= n_points]]))
    return np.setdiff1d(clusters, has_cluster_child)


class TestTheCondensedTree(unittest.TestCase):

    def setUp(self):
        self.x, self.truth = three_blobs()
        self.src, self.tgt, self.w = sorted_mst(self.x)
        self.parent, self.child, self.value, self.size = \
            tttrlib.hdbscan_condensed_tree(self.src, self.tgt, self.w, 5)
        self.n_points = len(self.x)

    def test_every_point_falls_out_of_exactly_one_cluster(self):
        """The structural invariant that makes the tree a partition."""
        points = self.child[self.child < self.n_points]
        self.assertEqual(points.size, self.n_points)
        np.testing.assert_array_equal(np.sort(points), np.arange(self.n_points))

    def test_a_parent_is_always_a_cluster_never_a_point(self):
        self.assertTrue((self.parent >= self.n_points).all())

    def test_the_root_is_the_point_count(self):
        """Node ids are renumbered so `n_samples` is the root — callers rely on
        it to tell an unclaimed point from a clustered one."""
        self.assertEqual(int(self.parent.min()), self.n_points)

    def test_a_cluster_child_never_falls_below_min_cluster_size(self):
        """The whole point of condensing: a split both sides survive."""
        sizes = self.size[self.child >= self.n_points]
        self.assertTrue((sizes >= 5).all(), "a split kept a side below the minimum")

    def test_lambda_is_one_over_the_merge_distance(self):
        finite = np.isfinite(self.value)
        self.assertTrue((self.value[finite] > 0).all())

    def test_unsorted_edges_are_rejected(self):
        """Linkage is order-dependent: unsorted input gives a plausible, wrong
        dendrogram rather than an error, so the error is made here."""
        shuffled = self.w.copy()
        shuffled[0], shuffled[-1] = shuffled[-1], shuffled[0]
        with self.assertRaises(ValueError):
            tttrlib.hdbscan_condensed_tree(self.src, self.tgt, shuffled, 5)

    def test_it_is_deterministic(self):
        again = tttrlib.hdbscan_condensed_tree(self.src, self.tgt, self.w, 5)
        for a, b in zip((self.parent, self.child, self.value, self.size), again):
            np.testing.assert_array_equal(a, b)


class TestLabellingRecoversTheBlobs(unittest.TestCase):
    """The method test: does clustering return the structure that was put in."""

    def setUp(self):
        self.x, self.truth = three_blobs()
        src, tgt, w = sorted_mst(self.x)
        self.parent, self.child, _, _ = tttrlib.hdbscan_condensed_tree(src, tgt, w, 5)
        self.n_points = len(self.x)
        self.selected = np.zeros(int(self.parent.max()) + 1, dtype=np.uint8)
        self.selected[leaf_clusters(self.parent, self.child, self.n_points)] = 1

    def roots(self):
        return np.asarray(tttrlib.hdbscan_label_points(
            self.parent, self.child, self.selected, self.n_points))

    def test_no_cluster_mixes_two_blobs(self):
        """Purity, not count: leaf selection deliberately over-fragments, so
        "three clusters" is not the claim — "no cluster spans two blobs" is."""
        roots = self.roots()
        for root in np.unique(roots):
            if root == self.n_points:
                continue                      # unclaimed; the caller calls it noise
            members = self.truth[roots == root]
            self.assertEqual(np.unique(members).size, 1,
                             "cluster %d mixes blobs %s" % (root, np.unique(members)))

    def test_most_points_are_claimed(self):
        roots = self.roots()
        claimed = (roots != self.n_points).mean()
        self.assertGreater(claimed, 0.1, "almost nothing was clustered")

    def test_selecting_nothing_leaves_every_point_at_the_root(self):
        self.selected[:] = 0
        np.testing.assert_array_equal(
            self.roots(), np.full(self.n_points, self.n_points, dtype=np.int64))

    def test_a_selected_cluster_is_its_own_root(self):
        """Nothing above a selected cluster may absorb it — that is what lets a
        label be read off at all."""
        roots = self.roots()
        for root in np.unique(roots):
            if root != self.n_points:
                self.assertTrue(bool(self.selected[root]),
                                "point landed on an unselected cluster %d" % root)

    def test_a_child_id_outside_the_selection_is_rejected(self):
        with self.assertRaises(ValueError):
            tttrlib.hdbscan_label_points(self.parent, self.child,
                                         np.zeros(3, dtype=np.uint8), self.n_points)


class TestSelectingClusters(unittest.TestCase):
    """`hdbscan_select_clusters`: the step between the other two."""

    def setUp(self):
        self.x, self.truth = three_blobs()
        src, tgt, w = sorted_mst(self.x)
        self.parent, self.child, self.value, self.size = \
            tttrlib.hdbscan_condensed_tree(src, tgt, w, 5)
        self.n_points = len(self.x)

    def select(self, method="eom", allow_single_cluster=False, epsilon=0.0,
               max_cluster_size=0):
        return np.asarray(tttrlib.hdbscan_select_clusters(
            self.parent, self.child, self.value, self.size, method,
            allow_single_cluster, epsilon, max_cluster_size))

    def ancestors(self, node):
        """Every cluster strictly above `node`, walking parents to the root."""
        parent_of = {int(c): int(p) for p, c in zip(self.parent, self.child)
                     if c >= self.n_points}
        out = []
        while node in parent_of:
            node = parent_of[node]
            out.append(node)
        return out

    def test_the_output_covers_every_node_id(self):
        """It is handed straight to `hdbscan_label_points`, which rejects a
        selection too short to cover the tree's child ids."""
        for method in ("eom", "leaf"):
            selected = self.select(method)
            self.assertGreater(len(selected), int(self.child.max()))
            self.assertGreater(len(selected), int(self.parent.max()))

    def test_nothing_below_the_root_is_selected_twice_over(self):
        """The selected clusters are an antichain: no selected cluster is an
        ancestor of another, or points would belong to two clusters at once."""
        for method in ("eom", "leaf"):
            with self.subTest(method=method):
                selected = self.select(method)
                chosen = set(np.flatnonzero(selected).tolist())
                for node in chosen:
                    self.assertFalse(chosen.intersection(self.ancestors(node)),
                                     "cluster %d sits under another selected one" % node)

    def test_only_points_are_never_selected(self):
        for method in ("eom", "leaf"):
            selected = self.select(method)
            self.assertFalse(selected[:self.n_points].any(),
                             "a point id was marked as a cluster")

    def test_the_root_is_selected_only_when_it_is_allowed(self):
        self.assertFalse(bool(self.select("eom")[self.n_points]))
        self.assertFalse(bool(self.select("leaf")[self.n_points]))

    def test_leaf_selection_picks_only_leaves(self):
        """Leaf selection follows the density peaks, so every cluster it keeps
        is one that never split."""
        has_cluster_child = set(np.unique(
            self.parent[self.child >= self.n_points]).tolist())
        for node in np.flatnonzero(self.select("leaf")):
            self.assertNotIn(int(node), has_cluster_child)

    def test_leaf_selection_never_keeps_fewer_than_excess_of_mass(self):
        """Excess of mass takes a parent whenever the parent holds more mass
        than its children; leaf selection cannot, so it fragments at least as
        much. Counting is the only claim -- which clusters differ is the point
        of having two policies."""
        self.assertGreaterEqual(int(self.select("leaf").sum()),
                                int(self.select("eom").sum()))

    def test_selection_and_labelling_recover_the_blobs(self):
        """The end-to-end claim: the three calls compose into a clustering.

        Purity is required of both policies; coverage is not the same claim for
        both. Excess of mass takes whole blobs, so nearly every point is
        claimed. Leaf selection takes the density peaks, and a point that fell
        out of a blob before its peak formed is left at the root -- so it
        claims a third here, and that is the policy working, not failing.
        """
        for method, floor in (("eom", 0.8), ("leaf", 0.2)):
            with self.subTest(method=method):
                selected = self.select(method)
                roots = np.asarray(tttrlib.hdbscan_label_points(
                    self.parent, self.child, selected, self.n_points))
                claimed = roots != self.n_points
                self.assertGreater(claimed.mean(), floor)
                for root in np.unique(roots[claimed]):
                    members = np.unique(self.truth[roots == root])
                    self.assertEqual(members.size, 1,
                                     "cluster %d mixes blobs %s" % (root, members))

    def test_a_larger_epsilon_cannot_produce_more_clusters(self):
        """The epsilon walk-up only ever replaces clusters by an ancestor."""
        counts = [int(self.select("leaf", epsilon=e).sum()) for e in (0.0, 0.5, 2.0, 8.0)]
        self.assertEqual(counts, sorted(counts, reverse=True), counts)

    def test_max_cluster_size_rejects_the_big_candidates(self):
        """A candidate over the limit loses to its descendants, so nothing
        selected can be larger than it."""
        size_of = {int(c): int(s) for c, s in zip(self.child, self.size)
                   if c >= self.n_points}
        selected = self.select("eom", max_cluster_size=60)
        for node in np.flatnonzero(selected):
            self.assertLessEqual(size_of[int(node)], 60)

    def test_an_unknown_method_is_rejected(self):
        with self.assertRaises(Exception):
            self.select("excess_of_mass")

    def test_a_negative_epsilon_is_rejected(self):
        with self.assertRaises(Exception):
            self.select("eom", epsilon=-1.0)


class TestMembershipStrengths(unittest.TestCase):
    """`hdbscan_membership_strengths`: how firmly a point sits in its cluster."""

    def setUp(self):
        self.x, self.truth = three_blobs()
        src, tgt, w = sorted_mst(self.x)
        self.parent, self.child, self.value, self.size = \
            tttrlib.hdbscan_condensed_tree(src, tgt, w, 5)
        self.n_points = len(self.x)
        selected = np.asarray(tttrlib.hdbscan_select_clusters(
            self.parent, self.child, self.value, self.size, "eom", False, 0.0, 0))
        self.roots = np.asarray(tttrlib.hdbscan_label_points(
            self.parent, self.child, selected, self.n_points))
        self.strength = np.asarray(tttrlib.hdbscan_membership_strengths(
            self.parent, self.child, self.value, self.roots))

    def test_one_strength_per_point_inside_the_unit_interval(self):
        self.assertEqual(self.strength.shape, (self.n_points,))
        self.assertTrue((self.strength >= 0).all() and (self.strength <= 1).all())

    def test_a_cluster_reaches_full_strength_somewhere(self):
        """The last point to leave a cluster defines its death, so it scores 1
        -- a cluster whose best point scored below one would mean the death
        lambda came from somewhere else."""
        claimed = self.roots != self.n_points
        for root in np.unique(self.roots[claimed]):
            self.assertAlmostEqual(float(self.strength[self.roots == root].max()), 1.0)

    def test_the_weakest_members_are_the_ones_at_the_edge(self):
        """The score is a density ratio, so within a blob it must fall off with
        distance from the blob's centre. Correlation, not an ordering: the
        lambda a point leaves at is a density estimate, not a radius."""
        claimed = self.roots != self.n_points
        for root in np.unique(self.roots[claimed]):
            members = self.roots == root
            if members.sum() < 20:
                continue
            radius = np.linalg.norm(self.x[members] - self.x[members].mean(0), axis=1)
            corr = np.corrcoef(radius, self.strength[members])[0, 1]
            self.assertLess(corr, -0.2, "strength does not fall off outward (%.2f)" % corr)

    def test_roots_of_the_wrong_length_are_rejected(self):
        with self.assertRaises(Exception):
            tttrlib.hdbscan_membership_strengths(
                self.parent, self.child, self.value, self.roots[:-1])


class TestWhatPersistenceDoesAndDoesNotSay(unittest.TestCase):
    """Persistence ranks clusters by how much of the density range they survive
    — which is *not* the same as whether a cluster is one population.

    This exists because the opposite was asserted, in this repository's own
    documentation, on nothing but plausibility: a group that is really two
    overlapping populations sounded like it ought to look ephemeral. It does
    not. Two overlapping populations are one density mode, they persist as one
    — that is precisely why excess of mass merges them — and the merged
    mixture can outscore a genuinely pure cluster. The question does have an
    answer in the condensed tree, and it is the second test here.
    """

    @staticmethod
    def two_overlapping_populations(seed, separation):
        """Two FRET populations that overlap, plus a well-separated third.

        Excess of mass merges the overlapping pair into one 50 %-pure cluster
        and keeps the third whole, so each run yields one cluster that is
        certainly a mixture and one that is certainly not.
        """
        rng = np.random.default_rng(seed)

        def population(n, e, s, tau):
            return np.column_stack([rng.normal(e, 0.055, n), rng.normal(s, 0.04, n),
                                    rng.normal(tau, 0.28, n)])

        x = np.vstack([population(700, 0.5 - separation / 2, 0.55, 2.6),
                       population(700, 0.5 + separation / 2, 0.55, 2.1),
                       population(300, 0.05, 0.92, 3.9)])
        truth = np.repeat([0, 1, 2], [700, 700, 300])
        return np.ascontiguousarray((x - x.mean(0)) / x.std(0)), truth

    def clusters(self, x, truth):
        """Per selected cluster: purity, persistence, and its cluster children."""
        n_points = len(x)
        min_cluster_size = int(0.04 * n_points)
        mst = np.asarray(tttrlib.mutual_reachability_mst(x, 10, 1.0))
        parent, child, value, size = tttrlib.hdbscan_condensed_tree(
            np.ascontiguousarray(mst[:, 0].astype(np.int64)),
            np.ascontiguousarray(mst[:, 1].astype(np.int64)),
            np.ascontiguousarray(mst[:, 2]), min_cluster_size)
        selected = tttrlib.hdbscan_select_clusters(
            parent, child, value, size, "eom", False, 0.0, 0)
        roots = np.asarray(tttrlib.hdbscan_label_points(
            parent, child, selected, n_points))
        result = tttrlib.hdbscan(x, min_cluster_size, 10, 1.0, "eom")
        out = []
        for label, root in enumerate(np.unique(roots[roots != n_points])):
            members = result.labels == label
            counts = np.bincount(truth[members], minlength=3)
            out.append({
                "purity": counts.max() / counts.sum(),
                "persistence": float(result.persistence[label]),
                "n_cluster_children": int(((parent == root) & (child >= n_points)).sum()),
            })
        return out

    def test_persistence_does_not_rank_a_mixture_below_a_pure_cluster(self):
        """The negative result, pinned so the claim cannot creep back.

        Not "the mixture always wins" — that would be over-claiming a
        simulation. The claim is the weaker and sufficient one: there is no
        threshold, because the ranges overlap and the mixture is often on top.
        """
        mixed, pure = [], []
        for separation in (0.14, 0.18, 0.24):
            for seed in (1, 2, 3):
                x, truth = self.two_overlapping_populations(seed, separation)
                for cluster in self.clusters(x, truth):
                    (mixed if cluster["purity"] < 0.9 else pure).append(
                        cluster["persistence"])
        self.assertTrue(mixed and pure, "the geometry did not produce both kinds")
        self.assertGreater(max(mixed), min(pure),
                           "persistence separated mixture from population — if this "
                           "is now reliably true the documentation should say so, "
                           "which it deliberately does not")

    def test_a_merged_mixture_is_the_cluster_with_cluster_children(self):
        """The positive result: the condensed tree does answer the question.

        A selected cluster that merged two density modes still has both of them
        below it as cluster children; a single population has none. The caller
        already has the arrays this reads.
        """
        for separation in (0.14, 0.18, 0.24):
            for seed in (1, 2, 3):
                with self.subTest(separation=separation, seed=seed):
                    x, truth = self.two_overlapping_populations(seed, separation)
                    for cluster in self.clusters(x, truth):
                        if cluster["purity"] < 0.9:
                            self.assertGreaterEqual(
                                cluster["n_cluster_children"], 2,
                                "a 50%-pure merge with no cluster children")
                        else:
                            self.assertEqual(
                                cluster["n_cluster_children"], 0,
                                "a pure population that splits further")


if __name__ == "__main__":
    unittest.main()
