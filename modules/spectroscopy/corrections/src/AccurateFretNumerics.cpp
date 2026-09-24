// SPDX-License-Identifier: BSD-3-Clause
// numpy-exact reductions for the accurate-FRET kernels: the A/B reference is
// numpy, and a naive left-to-right sum already differs in the last bits at
// n ~ 100, which EM then carries into every later iteration
#include "AccurateFretDetail.h"

#include <algorithm>
#include <cmath>

namespace tttrlib {
namespace afret_detail {

double np_sum(const double* a, size_t n) {
    if (n < 8) {
        double res = 0.0;
        for (size_t i = 0; i < n; ++i) res += a[i];
        return res;
    }
    if (n <= 128) {
        double r[8];
        for (int j = 0; j < 8; ++j) r[j] = a[j];
        size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; ++j) r[j] += a[i + j];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) res += a[i];
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return np_sum(a, n2) + np_sum(a + n2, n - n2);
}

double np_std(const std::vector<double>& a) {
    const double mean = np_mean(a);
    std::vector<double> sq(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        double d = a[i] - mean;
        sq[i] = d * d;
    }
    return std::sqrt(np_sum(sq) / static_cast<double>(a.size()));
}

std::vector<double> np_quantile(std::vector<double> a, const std::vector<double>& q) {
    std::sort(a.begin(), a.end());
    const double n = static_cast<double>(a.size());
    const long last = static_cast<long>(a.size()) - 1;
    std::vector<double> out;
    for (double qq : q) {
        // numpy's _compute_virtual_index for alpha = beta = 1, term for term
        double vi = n * qq + (1.0 + qq * (1.0 - 1.0 - 1.0)) - 1.0;
        double prev_f = std::floor(vi);
        long prev = static_cast<long>(prev_f), next = prev + 1;
        if (vi >= n - 1) { prev = last; next = last; }
        if (vi < 0) { prev = 0; next = 0; }
        const double t = vi - prev_f;
        const double lo = a[prev], hi = a[next];
        const double diff = hi - lo;
        out.push_back(t >= 0.5 ? hi - diff * (1.0 - t) : lo + diff * t);
    }
    return out;
}

std::vector<double> np_linspace(double lo, double hi, int k) {
    std::vector<double> out(k);
    if (k == 1) { out[0] = lo; return out; }
    const double step = (hi - lo) / static_cast<double>(k - 1);
    for (int i = 0; i < k; ++i) out[i] = static_cast<double>(i) * step + lo;
    out[k - 1] = hi;
    return out;
}

double np_nanmean(const std::vector<double>& a) {
    std::vector<double> z(a.size());
    size_t cnt = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const bool nan = std::isnan(a[i]);
        z[i] = nan ? 0.0 : a[i];
        cnt += !nan;
    }
    return np_sum(z) / static_cast<double>(cnt);
}

double np_nanstd(const std::vector<double>& a) {
    const double avg = np_nanmean(a);
    std::vector<double> sq(a.size());
    size_t cnt = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i])) { sq[i] = 0.0; continue; }
        const double d = a[i] - avg;
        sq[i] = d * d;
        ++cnt;
    }
    return std::sqrt(np_sum(sq) / static_cast<double>(cnt));
}

std::vector<double> finite_only(const std::vector<double>& x) {
    std::vector<double> out;
    out.reserve(x.size());
    for (double v : x)
        if (std::isfinite(v)) out.push_back(v);
    return out;
}

} // namespace afret_detail
} // namespace tttrlib
