// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_LSAP_H
#define TTTRLIB_LSAP_H
// Validation: A/B-TESTED 2026-10-05 -- vs scipy.optimize.linear_sum_assignment
//   1.18.0: identical row/column pairs on random square, wide, tall, integer
//   (tie-heavy) and constant cost matrices, +inf entries and maximize=True
//   (test/python/misc/test_ndimage_parity.py).
//
// Lsap.h -- the rectangular linear sum assignment problem: pick one column
// per row (or one row per column, whichever side is shorter) so the summed
// cost is minimal.
//
// A port of scipy's rectangular_lsap.cpp (Crouse's shortest augmenting path,
// "On implementing 2D rectangular assignment algorithms", IEEE TAES 52(4)
// 1679-1696, 2016; implementation by PM Larsen, BSD-3-Clause, Copyright the
// SciPy developers), kept line for line because the *assignment* is part of
// the contract, not just the cost: with tied costs several assignments are
// optimal, and a tracker that links particle 3 to 7 under one library and to
// 8 under the other has changed its output. The reversed `remaining` fill
// and the prefer-a-free-column tie-break are what fix scipy's answer; both
// are here.

#include <cstdint>
#include <vector>

namespace tttrlib {

/// Solve the assignment for the row-major `n_rows x n_cols` `cost` matrix.
/// Returns the matched pairs as `rows` (ascending) and `cols`, of length
/// `min(n_rows, n_cols)`. `+inf` marks a forbidden pair. Throws
/// `std::invalid_argument` on NaN or `-inf` entries and when no finite
/// assignment exists.
void linear_sum_assignment(const double* cost, int n_rows, int n_cols, bool maximize,
                           std::vector<std::int64_t>& rows, std::vector<std::int64_t>& cols);

/// Binding facade: rows and columns concatenated, `2 * min(n_rows, n_cols)`
/// entries in a malloc'd buffer.
void lsap_solve(const double* cost, int n_rows, int n_cols, int maximize,
                long long** out, int* n_out);

}  // namespace tttrlib

#endif  // TTTRLIB_LSAP_H
