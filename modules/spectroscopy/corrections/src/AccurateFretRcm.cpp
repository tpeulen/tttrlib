// SPDX-License-Identifier: BSD-3-Clause
#include "AccurateFretCalibrate.h"
#include "Mat.h"

#include <algorithm>
#include <stdexcept>

namespace tttrlib {

std::vector<double> rcm_from_dye_solutions(const std::vector<double>& donor,
                                           const std::vector<double>& acceptor,
                                           double absorbance_ratio,
                                           const std::vector<std::string>& species,
                                           const std::vector<std::string>& polarisation,
                                           double r_donor, double r_acceptor) {
    const size_t nchtot = species.size();
    if (donor.size() != nchtot || acceptor.size() != nchtot)
        throw std::invalid_argument("rate vectors must match the number of detector channels");
    if (!polarisation.empty() && polarisation.size() != nchtot)
        throw std::invalid_argument("polarisation must name every detector channel");
    std::vector<int> a_idx, d_idx;
    for (size_t i = 0; i < nchtot; ++i) {
        if (species[i] == "A") a_idx.push_back(static_cast<int>(i));
        if (species[i] == "D") d_idx.push_back(static_cast<int>(i));
    }
    if (a_idx.size() != d_idx.size() || a_idx.empty() || a_idx.size() > 2)
        throw std::invalid_argument("need an equal number of A and D channels (1 or 2 each)");
    const int nch = static_cast<int>(a_idx.size() + d_idx.size());
    std::vector<int> order;
    double pa = 0.0, pd = 0.0;
    if (nch == 2) {
        order = {a_idx[0], d_idx[0]};
    } else {
        auto find = [&](const char* sp, const char* pl) {
            std::vector<int> hits;
            for (size_t i = 0; i < nchtot; ++i)
                if (species[i] == sp && !polarisation.empty() && polarisation[i] == pl)
                    hits.push_back(static_cast<int>(i));
            return hits;
        };
        const auto ap = find("A", "P"), dp = find("D", "P"), as = find("A", "S"), ds = find("D", "S");
        if (ap.size() == 1 && dp.size() == 1 && as.size() == 1 && ds.size() == 1) {
            order = {ap[0], dp[0], as[0], ds[0]};
            pa = (3.0 * r_acceptor) / (2.0 + r_acceptor);
            pd = (3.0 * r_donor) / (2.0 + r_donor);
        } else {  // 50/50 beam splitter, polarisation not resolved
            order = {a_idx[0], d_idx[0], a_idx[1], d_idx[1]};
        }
    }
    std::vector<double> nd(nch), na(nch);
    for (int i = 0; i < nch; ++i) { nd[i] = donor[order[i]]; na[i] = acceptor[order[i]]; }
    const double al = absorbance_ratio;
    std::vector<double> a(static_cast<size_t>(nch) * nch, 0.0);
    if (nch == 2) {
        a = {na[0], al * nd[0], na[1], al * nd[1]};
    } else {
        a = {na[0] / (1 + pa), al * nd[0] / (1 + pd), 0.0, 0.0,
             na[1] / (1 + pa), al * nd[1] / (1 + pd), 0.0, 0.0,
             0.0, 0.0, na[2] / (1 - pa), al * nd[2] / (1 - pd),
             0.0, 0.0, na[3] / (1 - pa), al * nd[3] / (1 - pd)};
    }
    if (!mat_inverse_inplace(a, nch)) throw std::invalid_argument("the dye-solution rate matrix is singular");
    const double norm = a[0];
    for (double& v : a) v /= norm;
    // scatter the (nch x nch) block back into channel space, identity elsewhere
    std::vector<double> rcm(nchtot * nchtot, 0.0);
    for (int i = 0; i < nch; ++i)
        for (int j = 0; j < nch; ++j) rcm[order[i] * nchtot + order[j]] = a[i * nch + j];
    for (size_t i = 0; i < nchtot; ++i)
        if (std::find(order.begin(), order.end(), static_cast<int>(i)) == order.end()) rcm[i * nchtot + i] = 1.0;
    return rcm;
}

} // namespace tttrlib
