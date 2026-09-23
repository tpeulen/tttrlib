// SPDX-License-Identifier: BSD-3-Clause
// internal helpers shared by the AccurateFret*.cpp translation units; not installed
#ifndef TTTRLIB_ACCURATEFRET_DETAIL_H
#define TTTRLIB_ACCURATEFRET_DETAIL_H

#include <vector>

namespace tttrlib {
namespace afret_detail {

void require_same_length(const std::vector<double>& a, const std::vector<double>& b,
                         const char* what);

// flat (donor, acceptor) pairs -> donors in first-appearance order, each with its
// acceptors in request order; empty pairs -> every i < j (i < n_first, j < n_second)
void group_pairs(const std::vector<int>& pairs, int n_first, int n_second,
                 std::vector<int>& donors, std::vector<std::vector<int>>& acceptors);

} // namespace afret_detail
} // namespace tttrlib

#endif
