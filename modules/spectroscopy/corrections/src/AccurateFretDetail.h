// SPDX-License-Identifier: BSD-3-Clause
// internal helpers shared by the AccurateFret*.cpp translation units; not installed
#ifndef TTTRLIB_ACCURATEFRET_DETAIL_H
#define TTTRLIB_ACCURATEFRET_DETAIL_H

#include <cstddef>
#include <string>
#include <vector>

#include "AccurateFretMultiDim.h"

namespace tttrlib {
namespace afret_detail {

void require_same_length(const std::vector<double>& a, const std::vector<double>& b,
                         const char* what);

// flat (donor, acceptor) pairs -> donors in first-appearance order, each with its
// acceptors in request order; empty pairs -> every i < j (i < n_first, j < n_second)
void group_pairs(const std::vector<int>& pairs, int n_first, int n_second,
                 std::vector<int>& donors, std::vector<std::vector<int>>& acceptors);

// numpy's float64 add.reduce of a contiguous 1-D array (pairwise, 8-way unrolled,
// blocks of 128): bit-identical to np.sum, so means match the reference exactly
double np_sum(const double* a, size_t n);
inline double np_sum(const std::vector<double>& a) { return np_sum(a.data(), a.size()); }
inline double np_mean(const std::vector<double>& a) { return np_sum(a) / static_cast<double>(a.size()); }
// np.std(a) (ddof 0)
double np_std(const std::vector<double>& a);
// np.quantile(a, q) with the default "linear" method; `a` need not be sorted
std::vector<double> np_quantile(std::vector<double> a, const std::vector<double>& q);
// np.linspace(lo, hi, k) with its endpoint
std::vector<double> np_linspace(double lo, double hi, int k);
// np.nanmean / np.nanstd (ddof 0): NaNs zeroed in place and summed with the rest,
// divided by the non-NaN count -- the exact reduction numpy performs
double np_nanmean(const std::vector<double>& a);
double np_nanstd(const std::vector<double>& a);
// np.interp(x, xp, fp) for one x (clamped at the ends, NaN for non-finite x)
double np_interp(double x, const std::vector<double>& xp, const std::vector<double>& fp);
// finite entries of x, in order
std::vector<double> finite_only(const std::vector<double>& x);

// classify_populations_nd's labelling, merging and purity rules applied to any
// set of components: `fit.labels` is the hard label per row (-1: in no
// component), `fit.responsibilities` the (n_rows, k) probabilities
PopulationSplit split_from_components(const MixtureNdResult& fit, int n_rows,
                                      const std::vector<std::string>& names,
                                      double donor_only_above, double acceptor_only_below,
                                      int min_population, double min_probability);

} // namespace afret_detail
} // namespace tttrlib

#endif
