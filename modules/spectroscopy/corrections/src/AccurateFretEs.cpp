// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFret.h"
#include "AccurateFretDetail.h"
#include "SpectralCrosstalk.h"

#include <cmath>
#include <stdexcept>

namespace tttrlib {

namespace afret_detail {

void require_same_length(const std::vector<double>& a, const std::vector<double>& b,
                         const char* what) {
    if (a.size() != b.size())
        throw std::invalid_argument(std::string(what) + ": per-burst arrays differ in length");
}

void group_pairs(const std::vector<int>& pairs, int n_first, int n_second,
                 std::vector<int>& donors, std::vector<std::vector<int>>& acceptors) {
    std::vector<int> flat = pairs;
    if (flat.empty())
        for (int i = 0; i < n_first; ++i)
            for (int j = i + 1; j < n_second; ++j) { flat.push_back(i); flat.push_back(j); }
    if (flat.size() % 2)
        throw std::invalid_argument("pairs must hold (donor, acceptor) index pairs");
    for (size_t p = 0; p < flat.size(); p += 2) {
        int i = flat[p], j = flat[p + 1];
        size_t slot = 0;
        while (slot < donors.size() && donors[slot] != i) ++slot;
        if (slot == donors.size()) { donors.push_back(i); acceptors.emplace_back(); }
        acceptors[slot].push_back(j);
    }
}

} // namespace afret_detail

using namespace afret_detail;

EsResult apparent_es(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                     const std::vector<double>& i_aa) {
    require_same_length(i_dd, i_da, "apparent_es");
    const size_t n = i_dd.size();
    EsResult r;
    r.has_s = !i_aa.empty() || n == 0;
    if (!i_aa.empty()) require_same_length(i_dd, i_aa, "apparent_es");
    r.E.resize(n);
    for (size_t b = 0; b < n; ++b) {
        double tot = i_dd[b] + i_da[b];
        r.E[b] = tot != 0.0 ? i_da[b] / tot : 0.0;
    }
    if (!i_aa.empty()) {
        r.S.resize(n);
        for (size_t b = 0; b < n; ++b) {
            double tot = i_dd[b] + i_da[b];
            double den = tot + i_aa[b];
            r.S[b] = den != 0.0 ? tot / den : 0.0;
        }
    }
    return r;
}

EsResult corrected_es(const std::vector<double>& i_dd, const std::vector<double>& i_da,
                      const std::vector<double>& i_aa, const FretFactors& f) {
    require_same_length(i_dd, i_da, "corrected_es");
    const bool alex = !i_aa.empty();
    if (alex) require_same_length(i_dd, i_aa, "corrected_es");
    const size_t n = i_dd.size();
    EsResult r;
    r.has_s = alex || n == 0;
    r.E.resize(n);
    r.fc.resize(n);
    if (alex) r.S.resize(n);
    for (size_t b = 0; b < n; ++b) {
        double f_dd = i_dd[b] - f.bg_dd;
        double f_aa = alex ? i_aa[b] - f.bg_aa : 0.0;
        // the three-cube kernel owns E and fc; its own S assumes beta = 1
        auto c = correct_three_cube(f_dd, i_da[b] - f.bg_da, f_aa, f.gamma, f.alpha, f.delta);
        r.E[b] = c[0];
        r.fc[b] = c[2];
        if (alex) {
            double num = f.gamma * f_dd + c[2];
            double den = num + f_aa / f.beta;
            r.S[b] = den != 0.0 ? num / den : 0.0;
        }
    }
    return r;
}

PairEsResult corrected_es_matrix(
    const std::vector<double>& intensity, int n, int n_bursts,
    const std::vector<double>& gamma, const std::vector<double>& alpha,
    const std::vector<double>& delta, const std::vector<double>& background,
    const std::vector<int>& pairs) {
    const size_t nn = static_cast<size_t>(n) * n;
    if (n <= 0 || n_bursts < 0 || intensity.size() != nn * n_bursts)
        throw std::invalid_argument("corrected_es_matrix: intensity must be (n, n, n_bursts)");
    if (gamma.size() != nn || alpha.size() != nn)
        throw std::invalid_argument("corrected_es_matrix: gamma and alpha must be (n, n)");
    if ((!delta.empty() && delta.size() != nn) || (!background.empty() && background.size() != nn))
        throw std::invalid_argument("corrected_es_matrix: delta and background must be (n, n)");
    auto at = [&](const std::vector<double>& m, int i, int j) {
        return m.empty() ? 0.0 : m[static_cast<size_t>(i) * n + j];
    };
    auto I = [&](int i, int j, int b) {
        return intensity[(static_cast<size_t>(i) * n + j) * n_bursts + b];
    };

    std::vector<int> donors;
    std::vector<std::vector<int>> acc;
    group_pairs(pairs, n, n, donors, acc);
    PairEsResult out;
    out.n_bursts = n_bursts;
    for (size_t d = 0; d < donors.size(); ++d) {
        int i = donors[d];
        for (int j : acc[d])
            if (i < 0 || i >= n || j < 0 || j >= n)
                throw std::invalid_argument("corrected_es_matrix: pair index out of range");
        std::vector<std::vector<double>> fc(acc[d].size(), std::vector<double>(n_bursts));
        std::vector<double> budget(n_bursts);
        for (int b = 0; b < n_bursts; ++b) {
            double f_ii = I(i, i, b) - at(background, i, i);
            double sum = 0.0;
            for (size_t a = 0; a < acc[d].size(); ++a) {
                int j = acc[d][a];
                fc[a][b] = (I(i, j, b) - at(background, i, j))
                           - at(alpha, i, j) * f_ii
                           - at(delta, i, j) * (I(j, j, b) - at(background, j, j));
                sum += fc[a][b] / at(gamma, i, j);
            }
            budget[b] = f_ii + sum;
        }
        for (size_t a = 0; a < acc[d].size(); ++a) {
            int j = acc[d][a];
            out.donor.push_back(i);
            out.acceptor.push_back(j);
            for (int b = 0; b < n_bursts; ++b) {
                out.E.push_back(budget[b] != 0.0 ? (fc[a][b] / at(gamma, i, j)) / budget[b] : 0.0);
                out.fc.push_back(fc[a][b]);
            }
        }
    }
    return out;
}

} // namespace tttrlib
