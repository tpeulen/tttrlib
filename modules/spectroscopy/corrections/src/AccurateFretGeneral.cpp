// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFret.h"
#include "AccurateFretDetail.h"
#include "Mat.h"
#include "Nnls.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace tttrlib {

using namespace afret_detail;

namespace {

// (n_det, n_chrom) matrix U with e[l, :] = I[l, :] U: numpy's pinv(emission), or the
// Tikhonov (emission emission^T + ridge I)^-1 emission, transposed
std::vector<double> unmix_matrix(const std::vector<double>& emis, int nc, int nd, double ridge) {
    std::vector<double> U(static_cast<size_t>(nd) * nc, 0.0);
    if (ridge > 0.0) {
        std::vector<double> gram(static_cast<size_t>(nc) * nc, 0.0);
        for (int i = 0; i < nc; ++i)
            for (int j = 0; j < nc; ++j) {
                double s = 0.0;
                for (int m = 0; m < nd; ++m) s += emis[i * nd + m] * emis[j * nd + m];
                gram[i * nc + j] = s + (i == j ? ridge : 0.0);
            }
        for (int m = 0; m < nd; ++m) {
            std::vector<double> A = gram, b(nc);
            for (int k = 0; k < nc; ++k) b[k] = emis[k * nd + m];
            if (!mat_solve(A, b, nc))
                throw std::invalid_argument("corrected_es_general: ridge system is singular");
            for (int k = 0; k < nc; ++k) U[m * nc + k] = b[k];
        }
        return U;
    }
    // pinv(emission) = pinv(emission^T)^T, one minimum-norm solve per unit vector;
    // rcond 1e-15 is numpy.linalg.pinv's default
    for (int m = 0; m < nd; ++m) {
        std::vector<double> A(static_cast<size_t>(nd) * nc), b(nd, 0.0);
        for (int r = 0; r < nd; ++r)
            for (int k = 0; k < nc; ++k) A[r * nc + k] = emis[k * nd + r];
        b[m] = 1.0;
        mat_lstsq_minnorm(A, b, nd, nc, 1e-15);
        for (int k = 0; k < nc; ++k) U[m * nc + k] = b[k];
    }
    return U;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

PairEsResult corrected_es_general(
    const std::vector<double>& intensity, int n_lasers, int n_detectors, int n_bursts,
    const std::vector<double>& excitation, const std::vector<double>& emission,
    int n_chromophores, const std::vector<double>& background,
    const std::vector<int>& pairs, const std::string& unmix, double ridge) {
    const int L = n_lasers, M = n_detectors, N = n_chromophores, B = n_bursts;
    if (L <= 0 || M <= 0 || N <= 0 || B < 0)
        throw std::invalid_argument("corrected_es_general: dimensions must be positive");
    if (intensity.size() != static_cast<size_t>(L) * M * B)
        throw std::invalid_argument("corrected_es_general: intensity must be (n_lasers, n_detectors, n_bursts)");
    if (excitation.size() != static_cast<size_t>(L) * N)
        throw std::invalid_argument("corrected_es_general: excitation must be (n_lasers, n_chromophores)");
    if (emission.size() != static_cast<size_t>(N) * M)
        throw std::invalid_argument("corrected_es_general: emission must be (n_chromophores, n_detectors)");
    if (!background.empty() && background.size() != static_cast<size_t>(L) * M)
        throw std::invalid_argument("corrected_es_general: background must be (n_lasers, n_detectors)");

    auto I = [&](int l, int m, int b) {
        double v = intensity[(static_cast<size_t>(l) * M + m) * B + b];
        return background.empty() ? v : v - background[static_cast<size_t>(l) * M + m];
    };

    // step 1: e[l, k, b]
    std::vector<double> e(static_cast<size_t>(L) * N * B, 0.0);
    auto E_at = [&](int l, int k, int b) -> double& { return e[(static_cast<size_t>(l) * N + k) * B + b]; };
    const std::string method = lower(unmix);
    if (method == "naive" || method == "pinv" || method == "linear") {
        std::vector<double> U = unmix_matrix(emission, N, M, ridge);
        for (int l = 0; l < L; ++l)
            for (int k = 0; k < N; ++k)
                for (int b = 0; b < B; ++b) {
                    double s = 0.0;
                    for (int m = 0; m < M; ++m) s += I(l, m, b) * U[m * N + k];
                    E_at(l, k, b) = s;
                }
    } else if (method == "stable" || method == "nnls" || method == "nonneg") {
        const int rows = M + (ridge > 0.0 ? N : 0);
        std::vector<double> A(static_cast<size_t>(rows) * N, 0.0);
        for (int m = 0; m < M; ++m)
            for (int k = 0; k < N; ++k) A[m * N + k] = emission[k * M + m];
        for (int k = 0; ridge > 0.0 && k < N; ++k) A[(M + k) * N + k] = std::sqrt(ridge);
        std::vector<double> y(rows, 0.0);
        for (int l = 0; l < L; ++l)
            for (int b = 0; b < B; ++b) {
                for (int m = 0; m < M; ++m) y[m] = I(l, m, b);
                std::vector<double> x = nnls(A, y, rows, N);
                for (int k = 0; k < N; ++k) E_at(l, k, b) = x[k];
            }
    } else {
        throw std::invalid_argument("corrected_es_general: unmix must be 'naive' or 'stable' (got '" + unmix + "')");
    }

    std::vector<int> donors;
    std::vector<std::vector<int>> acc;
    group_pairs(pairs, L, N, donors, acc);
    PairEsResult out;
    out.n_bursts = B;
    for (size_t d = 0; d < donors.size(); ++d) {
        const int i = donors[d];
        for (int j : acc[d])
            if (i < 0 || i >= L || i >= N || j < 0 || j >= N || j >= L)
                throw std::invalid_argument("corrected_es_general: pair index out of range");
        // step 2: remove the directly excited acceptor emission
        std::vector<std::vector<double>> fc(acc[d].size(), std::vector<double>(B));
        for (size_t a = 0; a < acc[d].size(); ++a) {
            const int j = acc[d][a];
            const double xjj = excitation[static_cast<size_t>(j) * N + j];
            const double x_rel = xjj != 0.0 ? excitation[static_cast<size_t>(i) * N + j] / xjj : 0.0;
            for (int b = 0; b < B; ++b) fc[a][b] = E_at(i, j, b) - x_rel * E_at(j, j, b);
        }
        // step 3: coupled donor budget
        for (size_t a = 0; a < acc[d].size(); ++a) {
            out.donor.push_back(i);
            out.acceptor.push_back(acc[d][a]);
            for (int b = 0; b < B; ++b) {
                double sum = 0.0;
                for (size_t c = 0; c < acc[d].size(); ++c) sum += fc[c][b];
                const double budget = E_at(i, i, b) + sum;
                out.E.push_back(budget != 0.0 ? fc[a][b] / budget : 0.0);
                out.fc.push_back(fc[a][b]);
            }
        }
    }
    return out;
}

} // namespace tttrlib
