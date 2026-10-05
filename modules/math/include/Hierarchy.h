// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_HIERARCHY_H
#define TTTRLIB_HIERARCHY_H
// Validation: A/B-TESTED 2026-10-05 -- vs scipy.cluster.hierarchy 1.18.0:
//   linkage matrices bit-identical for single/complete/average/weighted/ward
//   on random and tie-heavy (integer) condensed distances; fcluster labels
//   identical for distance, maxclust, inconsistent, monocrit and
//   maxclust_monocrit (test/python/misc/test_ndimage_parity.py).
//
// Hierarchy.h -- agglomerative clustering from a condensed distance matrix,
// and the flat cuts through the resulting dendrogram.
//
// Ports of scipy/cluster/hierarchy/_hierarchy.pyx (BSD-3-Clause, Copyright
// the SciPy developers): the nearest-neighbour chain for complete, average,
// weighted and ward linkage, Prim's MST for single linkage, the stable sort
// by merge height and the union-find relabelling that produce scipy's `Z`,
// and the stack walks behind `fcluster`. Copied rather than re-derived
// because cluster *numbers* are part of what a caller sees: two correct
// dendrograms can list their merges in different orders, and `fcluster`
// numbers clusters in the order its depth-first walk meets them.
//
// Z layout (scipy's): `(n - 1) x 4` row-major -- the two merged cluster ids
// (leaves are 0..n-1, merge k creates id n + k), the merge distance, the
// size of the new cluster.

#include <string>
#include <vector>

namespace tttrlib {

/// Linkage methods by scipy's codes: 0 single, 1 complete, 2 average,
/// 5 ward, 6 weighted. (3 centroid and 4 median are not implemented.)
std::vector<double> hierarchy_linkage(const std::vector<double>& condensed, int n, int method);

/// Inconsistency statistics `(n - 1) x 4` of a linkage matrix, `depth` levels deep.
std::vector<double> hierarchy_inconsistent(const std::vector<double>& Z, int n, int depth);

/// Flat cluster numbers (1-based) for each of the `n` observations.
/// `criterion`: "distance", "maxclust", "inconsistent", "monocrit",
/// "maxclust_monocrit". `R` (inconsistent) and `monocrit` may be empty when
/// the criterion does not use them; an empty `R` for "inconsistent" is
/// computed with `depth`.
std::vector<int> hierarchy_fcluster(const std::vector<double>& Z, int n, const std::string& criterion,
                                    double t, int depth, const std::vector<double>& R,
                                    const std::vector<double>& monocrit);

// --- binding facade ---------------------------------------------------------------

/// `hierarchy_linkage` over a condensed matrix; malloc'd `(n-1)*4` result.
void hierarchy_linkage_flat(const double* condensed, int n_condensed, int method, double** out, int* n_out);

/// `hierarchy_inconsistent`; malloc'd `(n-1)*4` result.
void hierarchy_inconsistent_flat(const double* Z, int n_Z, int depth, double** out, int* n_out);

/// `hierarchy_fcluster`; `R`/`monocrit` may be empty arrays.
void hierarchy_fcluster_flat(const double* Z, int n_Z, const std::string& criterion, double t, int depth,
                             const double* R, int n_R, const double* monocrit, int n_monocrit, int** out,
                             int* n_out);

}  // namespace tttrlib

#endif  // TTTRLIB_HIERARCHY_H
