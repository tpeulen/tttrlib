// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_CLUSTER_H
#define TTTRLIB_CLUSTER_H

// Validation: A/B-TESTED 2026-08-19 -- vs sklearn NearestNeighbors (core_distances, ulp), scipy
//   minimum_spanning_tree (weight multiset bit-identical) and sklearn.cluster.HDBSCAN (labels exact
//   from either side's tree; end-to-end differs only on MST ties). ChiSurf is NOT a reference.
//   test/python/misc/test_math_ab_clustering.py.
//   Benchmarked HDBSCAN pipeline vs sklearn on n=20k: 21x, identical partition (bench_sciref.py, check_sciref.py).
//   Register: okf/testing/math-kernel-validation.md

// Cluster.h -- k-d tree nearest neighbours, and the two kernels that HDBSCAN
// spends all of its time in.
//
// Why this lives in `math` and not in an analysis module: none of it knows what
// a photon is. A k-d tree over an (n x d) table of doubles is wanted in several
// places at once -- burst feature spaces, localisation tables, and the
// density-based clustering the analysis GUIs offer -- and writing it once here
// is what keeps a fourth copy from appearing.
//
// The two kernels are:
//
//   core_distances(X, k)        distance from every point to its k-th nearest
//                               neighbour, the local density estimate HDBSCAN
//                               is built on. O(n log n) through the tree.
//
//   mutual_reachability_mst()   minimum spanning tree of the graph whose edge
//                               weight is max(core_i, core_j, d(i,j)). Boruvka
//                               with tree pruning, O(n log n) in practice
//                               against the O(n^2) of the textbook Prim, which
//                               is kept beside it as the obviously-correct
//                               implementation the fast one is checked against.
//
// ---------------------------------------------------------------------------
// The tie-break is part of the contract
// ---------------------------------------------------------------------------
// Mutual-reachability weights tie constantly: whenever the max is a *core*
// distance, every edge that core distance dominates carries the same weight, so
// hundreds of edges can share one value. An MST is then not unique, and Boruvka
// and Prim pick different ones -- which changes the dendrogram, and with it the
// cluster count. Callers that can fall back to their own Prim (chisurf does)
// would silently get a different answer depending on whether this library was
// importable.
//
// So the edge order here is TOTAL: by weight, then by the sorted endpoint pair
// (min(u,v), max(u,v)). Under a total edge order the MST is unique and every
// correct algorithm returns the same one. `edge_less` below is that order and
// must stay identical to the caller's.
//
// The returned rows are *sorted* in it, too. Boruvka finds edges by round and
// Prim by chain, neither of which is an order anybody wants, and the very next
// step -- `hdbscan_condensed_tree` -- rejects an unsorted edge list because the
// linkage is order-dependent. Every caller was therefore sorting, and a caller
// that sorted by weight alone silently got a different dendrogram on tied
// weights than one that sorted by the full order. Sorting here ends that.

#include <cstddef>
#include <vector>

namespace tttrlib {

/// Total order on weighted edges: weight first, then the sorted endpoint pair.
/// Read the header comment before changing it -- it is a compatibility surface.
inline bool edge_less(double w1, int u1, int v1, double w2, int u2, int v2) {
    if (w1 != w2) return w1 < w2;
    const int a1 = u1 < v1 ? u1 : v1, b1 = u1 < v1 ? v1 : u1;
    const int a2 = u2 < v2 ? u2 : v2, b2 = u2 < v2 ? v2 : u2;
    if (a1 != a2) return a1 < a2;
    return b1 < b2;
}

/// A static k-d tree over a row-major (n_samples x n_features) table.
///
/// Built once and queried many times. The data is not copied; the caller must
/// keep it alive for the lifetime of the tree.
class KDTree {
public:
    /// Build the tree over a copy of `data`. `leaf_size` is the point count
    /// below which a node stops splitting; 32 keeps the traversal shallow
    /// without letting the linear scan inside a leaf dominate.
    ///
    /// The copy is deliberate: the tree outlives the call that built it, and
    /// the array it is built from is routinely a temporary owned by a language
    /// binding.
    KDTree(const double* data, int n_samples, int n_features, int leaf_size = 32);

    int n_samples() const { return n_samples_; }
    int n_features() const { return n_features_; }

    /// Squared distances and indices of the `k` nearest neighbours of `point`,
    /// including the point itself when it is part of the data. Both output
    /// buffers must hold `k` entries; results come back sorted ascending.
    void query(const double* point, int k, int* out_index, double* out_sq_dist) const;

    /// Distance to the `k`-th nearest neighbour of every point in the tree.
    /// `k` counts the point itself, so `k == 1` gives zeros.
    ///
    /// The `k` neighbours themselves are kept, because
    /// `mutual_reachability_mst` needs a cheap upper bound on each point's
    /// shortest outgoing edge and its own nearest neighbours are the best one
    /// available. Calling this before the MST is therefore not just the natural
    /// order, it is the fast path.
    std::vector<double> core_distances(int k) const;

    /// Minimum spanning tree of the mutual-reachability graph, as `3 * (n-1)`
    /// doubles laid out row-major as `[source, target, weight]` with
    /// `source < target`, sorted in the total edge order (`edge_less`) -- which
    /// is what the linkage downstream requires and what makes the result
    /// reproducible under ties.
    ///
    /// `core` must hold one core distance per point (typically from
    /// `core_distances`); `alpha` divides the plain distance before the
    /// inflation, so values above one make the hierarchy more conservative.
    ///
    /// Borůvka over the tree, `O(n log n)` while the tree prunes. This is the
    /// kernel to use; `mst_prim` returns the same tree and exists to check it.
    std::vector<double> mutual_reachability_mst(const std::vector<double>& core,
                                                double alpha = 1.0) const;

    /// The same tree, in the same order, by Prim's algorithm: `O(n^2 d)`, no
    /// tree, no pruning,
    /// nothing clever. It is kept because it is *obviously* correct, which
    /// makes it the thing the Borůvka above is checked against — the two must
    /// return the same tree edge for edge, and that is what proves the total
    /// edge order is doing its job. It was also the faster of the two above
    /// about ten dimensions until the tie comparison moved into distance
    /// space; that is no longer true at any dimension measured, so nothing
    /// dispatches to it.
    std::vector<double> mst_prim(const std::vector<double>& core,
                                 double alpha = 1.0) const;

    /// Leaf size to build with when the caller has no opinion.
    ///
    /// Small leaves pay off only while the bounding boxes are tight enough to
    /// prune: in two or three dimensions a 16-point leaf beats a 128-point one
    /// by a third, and by sixteen dimensions the ranking has reversed, because
    /// every box is visited anyway and only the per-node overhead is left.
    static int default_leaf_size(int n_features) {
        if (n_features <= 4) return 16;
        if (n_features <= 10) return 32;
        return 128;
    }

private:
    struct Node {
        int start = 0;   ///< first index into index_ owned by this node
        int stop = 0;    ///< one past the last
        int left = -1;   ///< child node, or -1 for a leaf
        int right = -1;
    };

    /// Squared distance from `point` to the bounding box of node `node`.
    double node_lower_bound(int node, const double* point) const;
    /// Recursive k-NN descent; `heap` holds the k best so far as a max-heap.
    void query_node(int node, const double* point, int k, int* out_index,
                    double* out_sq_dist, int& filled) const;

    int build(int start, int stop, int depth);

    std::vector<double> owned_;   ///< the copy the tree is built over
    const double* data_ = nullptr;
    int n_samples_ = 0;
    int n_features_ = 0;
    int leaf_size_ = 32;
    std::vector<int> index_;      ///< permutation of 0..n-1, leaves are ranges
    std::vector<Node> nodes_;
    std::vector<double> bounds_;  ///< per node: n_features lows then highs

    // Neighbour cache filled by core_distances() and read by the MST.
    mutable std::vector<int> knn_index_;
    mutable std::vector<double> knn_sq_dist_;
    mutable int knn_k_ = 0;         ///< neighbours stored per point
    mutable int knn_core_rank_ = 0; ///< which of them is the core distance
};

// ---------------------------------------------------------------------------
// Flat entry points (these are what the language bindings expose)
// ---------------------------------------------------------------------------

/// Distance from every row of `input` to its `k`-th nearest neighbour.
void core_distances(double* input, int n_input1, int n_input2, int k,
                    double** output, int* n_output);

/// Minimum spanning tree of the mutual-reachability graph of `input`.
/// The result is `(n_samples - 1) x 3`, each row `[source, target, weight]`,
/// in the total order documented at the top of this file.
void mutual_reachability_mst(double* input, int n_input1, int n_input2,
                             int min_samples, double alpha, double** output,
                             int* n_output1, int* n_output2);

}  // namespace tttrlib

// ---------------------------------------------------------------------------
// The other half of HDBSCAN: everything downstream of the MST
// ---------------------------------------------------------------------------
// `core_distances` and `mutual_reachability_mst` above are the first half. The
// rest of a run -- union-find linkage, dendrogram condensation, and reading a
// label off per point -- is 43% of the compiled time (measured at n=100,000:
// MST 117.6 ms against linkage 25.4 and condense+label 65.1), and none of it is
// expressible in an array language: union-find, a breadth-first tree walk and a
// dynamic compaction are pointer-chasing.
//
// **Three calls, not one, and the split is at the policy boundary.** *Cluster
// selection* sits between condensation and labelling and it is policy --
// excess-of-mass or leaf selection, `allow_single_cluster`,
// `cluster_selection_epsilon`, `max_cluster_size`. A caller with its own rule
// must be able to substitute it, so it is its own call rather than a flag
// buried in a monolith:
//
//   hdbscan_condensed_tree()  MST edges -> condensed (parent, child, lambda, size)
//   hdbscan_select_clusters() condensed tree + policy -> is_selected
//   hdbscan_label_points()    condensed tree + selection -> a root per point
//   hdbscan_membership_strengths()  ... + those roots -> a strength per point
//
// The middle call being *substitutable* is not the same as it being *absent*:
// the two published policies (Campello et al. 2013 §4 excess of mass, and leaf
// selection) ship here, because a caller who has to reimplement the paper to
// get a label out has no working clustering, only three-quarters of one. Each
// call is one loop, so nothing pointer-chasing crosses the language boundary.

namespace tttrlib {

/*!
 * \brief Single-linkage dendrogram from an MST, condensed at min_cluster_size.
 *
 * \param sources,targets,weights  the MST edge list, **ascending in weight**.
 *        Rejected otherwise: the linkage is order-dependent and unsorted input
 *        produces a plausible, wrong dendrogram rather than an error.
 * \param min_cluster_size  a split counts only when *both* sides hold at least
 *        this many points; otherwise the small side is recorded as points
 *        falling out of the surviving cluster, at that merge's lambda.
 * \param out_parent,out_child,out_value,out_size  the condensed edge list,
 *        allocated here, **in that order** -- a language binding returns them
 *        as a four-tuple `(parent, child, lambda, size)` and the order is the
 *        only thing naming them. One row per condensed edge:
 *        - `parent` is always a cluster; `n_samples` is the root and cluster
 *          ids run upward from it without gaps.
 *        - `child` is a cluster (`>= n_samples`) when the row is a split, and
 *          a point (`< n_samples`) when the row is that point falling out.
 *        - `lambda` is `1 / merge distance`, so it *increases* with depth
 *          (infinite for a zero distance, i.e. duplicate points).
 *        - `size` is the child's point count: 1 exactly on the point rows.
 *        Every point appears as a child exactly once.
 */
void hdbscan_condensed_tree(
        long long* sources, int n_sources,
        long long* targets, int n_targets,
        double* weights, int n_weights,
        int min_cluster_size,
        long long** out_parent, int* n_out_parent,
        long long** out_child, int* n_out_child,
        double** out_value, int* n_out_value,
        long long** out_size, int* n_out_size);

/*!
 * \brief Collapse unselected clusters into their parents; return each point's root.
 *
 * \param is_selected  one byte per node id; the caller's selection.
 * \param n_points     the point count, which is also the root cluster's id.
 * \param out          [n_points] root node per point, allocated here. A point
 *        whose root is the root cluster was not claimed by any selected
 *        cluster -- the caller maps that to noise.
 */
void hdbscan_label_points(
        long long* parents, int n_parents,
        long long* children, int n_children,
        unsigned char* is_selected, int n_is_selected,
        int n_points,
        long long** out, int* n_out);

/*!
 * \brief Which clusters of a condensed tree to keep: the `is_selected` that
 *        `hdbscan_label_points` expects.
 *
 * The published policies, both of Campello, Moulavi & Sander, *Density-based
 * clustering based on hierarchical density estimates*, PAKDD 2013 (§4 for the
 * first), with the same defaults and the same corner cases as
 * `sklearn.cluster.HDBSCAN` -- which is what the A/B test pins this against:
 *
 *   - `"eom"`  excess of mass. A cluster's stability is
 *     `sum over its rows of (lambda - lambda_birth) * size`; walking the tree
 *     bottom-up, a cluster is kept when its own stability is at least that of
 *     its selected descendants, and taking it discards everything below it.
 *   - `"leaf"` every leaf of the cluster tree, which fragments more and follows
 *     the density peaks rather than the mass.
 *
 * \param parents,children,lambdas,sizes  the condensed tree, as
 *        `hdbscan_condensed_tree` returned it. The root -- and so the point
 *        count -- is `min(parents)`; there is no `n_points` parameter because
 *        it would only be a second chance to disagree with the tree.
 * \param method  `"eom"` or `"leaf"`.
 * \param allow_single_cluster  let the root itself be selected, i.e. admit the
 *        answer "this is one cluster". Off by default in every implementation
 *        because with it on, data with no structure comes back as one cluster
 *        rather than as noise.
 * \param cluster_selection_epsilon  distance below which splits are not taken:
 *        a selected cluster is walked back up while its parent was born closer
 *        than this (Malzer & Baum 2020). `0.0` disables it.
 * \param max_cluster_size  points; a candidate above it is rejected in favour
 *        of its descendants (`"eom"` only). `0` disables it.
 * \param out_selected  `[max(node id) + 1]` bytes, allocated here: 1 where a
 *        node is a selected cluster. Sized so it covers every id in the tree,
 *        which is what `hdbscan_label_points` requires of it.
 */
void hdbscan_select_clusters(
        long long* parents, int n_parents,
        long long* children, int n_children,
        double* lambdas, int n_lambdas,
        long long* sizes, int n_sizes,
        const char* method,
        bool allow_single_cluster,
        double cluster_selection_epsilon,
        long long max_cluster_size,
        unsigned char** out_selected, int* n_out_selected);

/*!
 * \brief Each cluster's stability: the quantity excess of mass optimises.
 *
 * `sum over the cluster's rows of (lambda - lambda_birth) * size` -- the mass
 * of Campello, Moulavi & Sander's excess of mass, and the number the selection
 * weighs a cluster against its children with. Normalised (see below) it is how
 * much of the density range a cluster survives: near one, it exists at every
 * scale; near zero, only in a narrow band.
 *
 * **It is not a purity signal, and it does not tell a population from a
 * mixture.** Two overlapping populations form *one* density mode, and that mode
 * survives a wide range of densities -- which is exactly why excess of mass
 * merged them. Measured on simulated two-population FRET tables at three
 * separations: the merged, 50%-pure cluster scored 0.45-0.66 and the pure
 * single population 0.44-0.46, so the mixture was at the *top* of the ranking
 * and no threshold separated them. What does separate them is in the condensed
 * tree the caller already has: a selected cluster that is a merged mixture has
 * *cluster children* (rows whose parent is that cluster and whose child is
 * `>= n_points`) and a genuinely single one has none -- 12 of 12 in the same
 * simulation, with the children's share of the parent growing with separation.
 *
 * \param parents,children,lambdas,sizes  the condensed tree.
 * \param out_stability  `[max(node id) + 1]` doubles, allocated here, indexed
 *        by absolute node id exactly as `hdbscan_select_clusters`' output is:
 *        zero at every point id, the stability at every cluster id. So the
 *        roots that `hdbscan_label_points` returns index straight into it.
 *
 * **It scales with the cluster's size**, so it does not compare two clusters as
 * it stands -- a large loose group outweighs a small tight one. The comparable
 * form is the *persistence* the reference implementation reports:
 * `stability / (points in the cluster * max lambda in the tree)`, in `[0, 1]`,
 * which `tttrlib.hdbscan` returns. Where the tree holds duplicate points the
 * maximum lambda is infinite and that ratio is undefined; the convention there,
 * ours and the reference's, is 1.0.
 */
void hdbscan_cluster_stability(
        long long* parents, int n_parents,
        long long* children, int n_children,
        double* lambdas, int n_lambdas,
        long long* sizes, int n_sizes,
        double** out_stability, int* n_out_stability);

/*!
 * \brief How firmly each point belongs to the cluster it landed in.
 *
 * `lambda_point / lambda_death`, clamped to one: the density at which the point
 * left its cluster over the density at which that cluster died. This is
 * scikit-learn's `probabilities_` (it is not a probability), and it is what one
 * thresholds to drop the points at a cluster's edge.
 *
 * \param parents,children,lambdas  the condensed tree.
 * \param roots  `hdbscan_label_points`'s output: one node id per point.
 * \param out_strength  `[n_roots]` doubles in `[0, 1]`, allocated here.
 *
 * Every point is scored against the cluster it is rooted at, *including* points
 * still rooted at the root cluster. Whether those are noise is the caller's
 * policy -- the same policy that decided `allow_single_cluster` -- so zeroing
 * them is left to the caller rather than assumed here.
 *
 * **It is a rank within a cluster, not a confidence across clusters.** The
 * denominator is each cluster's own death, so every cluster reaches one
 * somewhere no matter how diffuse it is, and 0.9 in a broad cluster says
 * nothing about 0.9 in a tight one. This bites hardest when the threshold is
 * carried between selection policies: leaf selection's clusters are small and
 * dense, so their strengths pile up near one (a table where excess of mass
 * spreads them over 0.16-1.0 gives leaf 0.92-1.0), and a cut that drops a fifth
 * of the points under `"eom"` drops none under `"leaf"`. The clamp also puts
 * every point that outlived its cluster's death at exactly one. To compare
 * *across* clusters, use the raw lambda a point left at -- the condensed tree's
 * `lambda` on the row where the point is the child -- which is a density in the
 * data's own units.
 */
void hdbscan_membership_strengths(
        long long* parents, int n_parents,
        long long* children, int n_children,
        double* lambdas, int n_lambdas,
        long long* roots, int n_roots,
        double** out_strength, int* n_out_strength);

}  // namespace tttrlib

#endif  // TTTRLIB_CLUSTER_H
