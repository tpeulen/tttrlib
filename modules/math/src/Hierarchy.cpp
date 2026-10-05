// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from scipy/cluster/hierarchy/_hierarchy.pyx and
// _hierarchy_distance_update.pxi (BSD-3-Clause, Copyright the SciPy
// developers). See Hierarchy.h.

#include "Hierarchy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>

namespace tttrlib {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

inline std::int64_t condensed_index(std::int64_t n, std::int64_t i, std::int64_t j) {
    if (i < j) return n * i - (i * (i + 1) / 2) + (j - i - 1);
    return n * j - (j * (j + 1) / 2) + (i - j - 1);
}

double update_distance(int method, double d_xi, double d_yi, double d_xy, int size_x, int size_y, int size_i) {
    switch (method) {
        case 0: return std::min(d_xi, d_yi);
        case 1: return std::max(d_xi, d_yi);
        case 2: return (size_x * d_xi + size_y * d_yi) / (size_x + size_y);
        case 5: {
            const double t = 1.0 / (size_x + size_y + size_i);
            return std::sqrt((size_i + size_x) * t * d_xi * d_xi + (size_i + size_y) * t * d_yi * d_yi -
                             size_i * t * d_xy * d_xy);
        }
        case 6: return 0.5 * (d_xi + d_yi);
        default: throw std::invalid_argument("linkage: method not implemented");
    }
}

int n_from_condensed(size_t m) {
    // m = n (n - 1) / 2
    const double d = std::ceil(std::sqrt(static_cast<double>(m) * 2));
    const auto n = static_cast<std::int64_t>(d);
    if (n * (n - 1) / 2 != static_cast<std::int64_t>(m))
        throw std::invalid_argument("linkage: length of a condensed distance matrix must be n(n-1)/2");
    return static_cast<int>(n);
}

/// Stable sort of Z's rows by merge distance, then scipy's union-find relabelling.
void sort_and_label(std::vector<double>& Z, int n) {
    std::vector<int> order(static_cast<size_t>(n - 1));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return Z[a * 4 + 2] < Z[b * 4 + 2]; });
    std::vector<double> sorted(Z.size());
    for (int k = 0; k < n - 1; ++k)
        for (int c = 0; c < 4; ++c) sorted[k * 4 + c] = Z[order[k] * 4 + c];
    Z.swap(sorted);

    std::vector<int> parent(static_cast<size_t>(2 * n - 1)), size(static_cast<size_t>(2 * n - 1), 1);
    std::iota(parent.begin(), parent.end(), 0);
    int next_label = n;
    auto find = [&](int x) {
        int root = x;
        while (parent[root] != root) root = parent[root];
        while (parent[x] != root) {
            const int nxt = parent[x];
            parent[x] = root;
            x = nxt;
        }
        return root;
    };
    for (int i = 0; i < n - 1; ++i) {
        const int x = static_cast<int>(Z[i * 4 + 0]);
        const int y = static_cast<int>(Z[i * 4 + 1]);
        const int xr = find(x), yr = find(y);
        if (xr < yr) {
            Z[i * 4 + 0] = xr;
            Z[i * 4 + 1] = yr;
        } else {
            Z[i * 4 + 0] = yr;
            Z[i * 4 + 1] = xr;
        }
        parent[xr] = next_label;
        parent[yr] = next_label;
        size[next_label] = size[xr] + size[yr];
        Z[i * 4 + 3] = size[next_label];
        ++next_label;
    }
}

std::vector<double> mst_single_linkage(const std::vector<double>& dists, int n) {
    std::vector<double> Z(static_cast<size_t>((n - 1) * 4), 0.0);
    std::vector<int> merged(static_cast<size_t>(n), 0);
    std::vector<double> D(static_cast<size_t>(n), kInf);
    int x = 0, y = 0;
    for (int k = 0; k < n - 1; ++k) {
        double current_min = kInf;
        merged[x] = 1;
        for (int i = 0; i < n; ++i) {
            if (merged[i] == 1) continue;
            const double dist = dists[static_cast<size_t>(condensed_index(n, x, i))];
            if (D[i] > dist) D[i] = dist;
            if (D[i] < current_min) {
                y = i;
                current_min = D[i];
            }
        }
        Z[k * 4 + 0] = x;
        Z[k * 4 + 1] = y;
        Z[k * 4 + 2] = current_min;
        x = y;
    }
    sort_and_label(Z, n);
    return Z;
}

std::vector<double> nn_chain(const std::vector<double>& dists, int n, int method) {
    std::vector<double> Z(static_cast<size_t>((n - 1) * 4), 0.0);
    std::vector<double> D = dists;
    std::vector<int> size(static_cast<size_t>(n), 1), chain(static_cast<size_t>(n));
    int chain_length = 0;
    int x = 0, y = 0;
    for (int k = 0; k < n - 1; ++k) {
        if (chain_length == 0) {
            chain_length = 1;
            for (int i = 0; i < n; ++i) {
                if (size[i] > 0) {
                    chain[0] = i;
                    break;
                }
            }
        }
        double current_min;
        while (true) {
            x = chain[chain_length - 1];
            // prefer the previous chain element as the minimum, to avoid cycles
            if (chain_length > 1) {
                y = chain[chain_length - 2];
                current_min = D[static_cast<size_t>(condensed_index(n, x, y))];
            } else {
                current_min = kInf;
            }
            for (int i = 0; i < n; ++i) {
                if (size[i] == 0 || x == i) continue;
                const double dist = D[static_cast<size_t>(condensed_index(n, x, i))];
                if (dist < current_min) {
                    current_min = dist;
                    y = i;
                }
            }
            if (chain_length > 1 && y == chain[chain_length - 2]) break;
            chain[chain_length] = y;
            chain_length += 1;
        }
        chain_length -= 2;
        if (x > y) std::swap(x, y);  // fastcluster's convention
        const int nx = size[x];
        const int ny = size[y];
        Z[k * 4 + 0] = x;
        Z[k * 4 + 1] = y;
        Z[k * 4 + 2] = current_min;
        Z[k * 4 + 3] = nx + ny;
        size[x] = 0;
        size[y] = nx + ny;
        for (int i = 0; i < n; ++i) {
            const int ni = size[i];
            if (ni == 0 || i == y) continue;
            D[static_cast<size_t>(condensed_index(n, i, y))] =
                update_distance(method, D[static_cast<size_t>(condensed_index(n, i, x))],
                                D[static_cast<size_t>(condensed_index(n, i, y))], current_min, nx, ny, ni);
        }
    }
    sort_and_label(Z, n);
    return Z;
}

// --- stack walks behind fcluster ------------------------------------------------

std::vector<double> max_dist_for_each_cluster(const std::vector<double>& Z, int n) {
    std::vector<double> MD(static_cast<size_t>(n - 1), 0.0);
    std::vector<int> curr(static_cast<size_t>(n));
    std::vector<char> visited(static_cast<size_t>(2 * n - 1), 0);
    int k = 0;
    curr[0] = 2 * n - 2;
    while (k >= 0) {
        const int root = curr[k] - n;
        const int lc = static_cast<int>(Z[root * 4 + 0]);
        const int rc = static_cast<int>(Z[root * 4 + 1]);
        if (lc >= n && !visited[lc]) {
            visited[lc] = 1;
            curr[++k] = lc;
            continue;
        }
        if (rc >= n && !visited[rc]) {
            visited[rc] = 1;
            curr[++k] = rc;
            continue;
        }
        double max_dist = Z[root * 4 + 2];
        if (lc >= n && MD[lc - n] > max_dist) max_dist = MD[lc - n];
        if (rc >= n && MD[rc - n] > max_dist) max_dist = MD[rc - n];
        MD[root] = max_dist;
        k -= 1;
    }
    return MD;
}

std::vector<double> max_rfield_for_each_cluster(const std::vector<double>& Z, const std::vector<double>& R,
                                                int n, int rf) {
    std::vector<double> out(static_cast<size_t>(n - 1), 0.0);
    std::vector<int> curr(static_cast<size_t>(n));
    std::vector<char> visited(static_cast<size_t>(2 * n - 1), 0);
    int k = 0;
    curr[0] = 2 * n - 2;
    while (k >= 0) {
        const int root = curr[k] - n;
        const int lc = static_cast<int>(Z[root * 4 + 0]);
        const int rc = static_cast<int>(Z[root * 4 + 1]);
        if (lc >= n && !visited[lc]) {
            visited[lc] = 1;
            curr[++k] = lc;
            continue;
        }
        if (rc >= n && !visited[rc]) {
            visited[rc] = 1;
            curr[++k] = rc;
            continue;
        }
        double m = R[root * 4 + rf];
        if (lc >= n && out[lc - n] > m) m = out[lc - n];
        if (rc >= n && out[rc - n] > m) m = out[rc - n];
        out[root] = m;
        k -= 1;
    }
    return out;
}

void cluster_monocrit(const std::vector<double>& Z, const std::vector<double>& MC, std::vector<int>& T,
                      double cutoff, int n) {
    int n_cluster = 0, cluster_leader = -1;
    std::vector<int> curr(static_cast<size_t>(n));
    std::vector<char> visited(static_cast<size_t>(2 * n - 1), 0);
    int k = 0;
    curr[0] = 2 * n - 2;
    while (k >= 0) {
        const int root = curr[k] - n;
        const int lc = static_cast<int>(Z[root * 4 + 0]);
        const int rc = static_cast<int>(Z[root * 4 + 1]);
        if (cluster_leader == -1 && MC[root] <= cutoff) {
            cluster_leader = root;
            n_cluster += 1;
        }
        if (lc >= n && !visited[lc]) {
            visited[lc] = 1;
            curr[++k] = lc;
            continue;
        }
        if (rc >= n && !visited[rc]) {
            visited[rc] = 1;
            curr[++k] = rc;
            continue;
        }
        if (lc < n) {
            if (cluster_leader == -1) n_cluster += 1;
            T[lc] = n_cluster;
        }
        if (rc < n) {
            if (cluster_leader == -1) n_cluster += 1;
            T[rc] = n_cluster;
        }
        if (cluster_leader == root) cluster_leader = -1;
        k -= 1;
    }
}

void cluster_maxclust_monocrit(const std::vector<double>& Z, const std::vector<double>& MC, std::vector<int>& T,
                               int n, int max_nc) {
    if (max_nc >= n) {
        for (int i = 0; i < n; ++i) T[i] = i + 1;
        return;
    }
    std::vector<int> curr(static_cast<size_t>(n));
    std::vector<char> visited(static_cast<size_t>(2 * n - 1));
    int lower_idx = -1;
    int upper_idx = n - 1;
    while (upper_idx - lower_idx > 1) {
        const int i = (lower_idx + upper_idx) >> 1;
        const double thresh = MC[i];
        std::fill(visited.begin(), visited.end(), 0);
        int nc = 0;
        int k = 0;
        curr[0] = 2 * n - 2;
        while (k >= 0) {
            const int root = curr[k] - n;
            const int lc = static_cast<int>(Z[root * 4 + 0]);
            const int rc = static_cast<int>(Z[root * 4 + 1]);
            if (MC[root] <= thresh) {
                nc += 1;
                if (nc > max_nc) break;
                k -= 1;
                visited[lc] = 1;
                visited[rc] = 1;
                continue;
            }
            if (!visited[lc]) {
                visited[lc] = 1;
                if (lc >= n) {
                    curr[++k] = lc;
                    continue;
                } else {
                    nc += 1;
                    if (nc > max_nc) break;
                }
            }
            if (!visited[rc]) {
                visited[rc] = 1;
                if (rc >= n) {
                    curr[++k] = rc;
                    continue;
                } else {
                    nc += 1;
                    if (nc > max_nc) break;
                }
            }
            k -= 1;
        }
        if (nc > max_nc) lower_idx = i;
        else upper_idx = i;
    }
    cluster_monocrit(Z, MC, T, MC[upper_idx], n);
}

void check_linkage(const std::vector<double>& Z, int n) {
    if (n < 2 || Z.size() != static_cast<size_t>((n - 1) * 4))
        throw std::invalid_argument("linkage matrix must be (n - 1) x 4 with n >= 2");
    for (int i = 0; i < n - 1; ++i) {
        const double a = Z[i * 4 + 0], b = Z[i * 4 + 1];
        if (a < 0 || b < 0 || a >= n + i || b >= n + i)
            throw std::invalid_argument("linkage matrix uses a cluster before it is formed");
    }
}

}  // namespace

std::vector<double> hierarchy_linkage(const std::vector<double>& condensed, int n, int method) {
    if (n < 2) throw std::invalid_argument("linkage: at least two observations are needed");
    if (condensed.size() != static_cast<size_t>(n) * (n - 1) / 2)
        throw std::invalid_argument("linkage: condensed matrix length does not match n");
    for (double d : condensed)
        if (!std::isfinite(d)) throw std::invalid_argument("The condensed distance matrix must contain only finite values.");
    switch (method) {
        case 0: return mst_single_linkage(condensed, n);
        case 1:
        case 2:
        case 5:
        case 6: return nn_chain(condensed, n, method);
        default: throw std::invalid_argument("linkage: method not implemented (centroid/median)");
    }
}

std::vector<double> hierarchy_inconsistent(const std::vector<double>& Z, int n, int depth) {
    check_linkage(Z, n);
    if (depth < 0) throw std::invalid_argument("inconsistent: depth must be non-negative");
    std::vector<double> R(static_cast<size_t>((n - 1) * 4), 0.0);
    std::vector<int> curr(static_cast<size_t>(n));
    std::vector<char> visited(static_cast<size_t>(2 * n - 1));
    for (int i = 0; i < n - 1; ++i) {
        int k = 0, level_count = 0;
        double level_sum = 0, level_std_sum = 0, level_std;
        std::fill(visited.begin(), visited.end(), 0);
        curr[0] = i;
        while (k >= 0) {
            const int root = curr[k];
            if (k < depth - 1) {
                const int lc = static_cast<int>(Z[root * 4 + 0]);
                if (lc >= n && !visited[lc]) {
                    visited[lc] = 1;
                    curr[++k] = lc - n;
                    continue;
                }
                const int rc = static_cast<int>(Z[root * 4 + 1]);
                if (rc >= n && !visited[rc]) {
                    visited[rc] = 1;
                    curr[++k] = rc - n;
                    continue;
                }
            }
            const double dist = Z[root * 4 + 2];
            level_count += 1;
            level_sum += dist;
            level_std_sum += dist * dist;
            k -= 1;
        }
        R[i * 4 + 0] = level_sum / level_count;
        R[i * 4 + 2] = level_count;
        if (level_count < 2) {
            level_std = (level_std_sum - (level_sum * level_sum)) / level_count;
        } else {
            level_std = ((level_std_sum - ((level_sum * level_sum) / level_count)) / (level_count - 1));
        }
        if (level_std > 0) {
            level_std = std::sqrt(level_std);
            R[i * 4 + 1] = level_std;
            R[i * 4 + 3] = (Z[i * 4 + 2] - R[i * 4 + 0]) / level_std;
        } else {
            R[i * 4 + 1] = 0;
        }
    }
    return R;
}

std::vector<int> hierarchy_fcluster(const std::vector<double>& Z, int n, const std::string& criterion,
                                    double t, int depth, const std::vector<double>& R,
                                    const std::vector<double>& monocrit) {
    check_linkage(Z, n);
    std::vector<int> T(static_cast<size_t>(n), 0);
    if ((criterion == "maxclust" || criterion == "maxclust_monocrit") && t < 1)
        throw std::invalid_argument("'" + criterion + "' needs 't', the maximum number of clusters, to be at least 1");
    if (criterion == "inconsistent") {
        const std::vector<double> RR = R.empty() ? hierarchy_inconsistent(Z, n, depth) : R;
        if (RR.size() != Z.size()) throw std::invalid_argument("inconsistency matrix must be (n - 1) x 4");
        cluster_monocrit(Z, max_rfield_for_each_cluster(Z, RR, n, 3), T, t, n);
    } else if (criterion == "distance") {
        cluster_monocrit(Z, max_dist_for_each_cluster(Z, n), T, t, n);
    } else if (criterion == "maxclust") {
        cluster_maxclust_monocrit(Z, max_dist_for_each_cluster(Z, n), T, n, static_cast<int>(t));
    } else if (criterion == "monocrit") {
        if (monocrit.size() != static_cast<size_t>(n - 1)) throw std::invalid_argument("monocrit must have n - 1 entries");
        cluster_monocrit(Z, monocrit, T, t, n);
    } else if (criterion == "maxclust_monocrit") {
        if (monocrit.size() != static_cast<size_t>(n - 1)) throw std::invalid_argument("monocrit must have n - 1 entries");
        cluster_maxclust_monocrit(Z, monocrit, T, n, static_cast<int>(t));
    } else {
        throw std::invalid_argument("Invalid cluster formation criterion: " + criterion);
    }
    return T;
}

// --- binding facade -------------------------------------------------------------------

namespace {
template <typename T>
T* copy_out(const std::vector<T>& v) {
    T* p = static_cast<T*>(std::malloc(sizeof(T) * (v.empty() ? 1 : v.size())));
    if (!p) throw std::bad_alloc();
    std::copy(v.begin(), v.end(), p);
    return p;
}
}  // namespace

void hierarchy_linkage_flat(const double* condensed, int n_condensed, int method, double** out, int* n_out) {
    const std::vector<double> d(condensed, condensed + n_condensed);
    const auto Z = hierarchy_linkage(d, n_from_condensed(d.size()), method);
    *out = copy_out(Z);
    *n_out = static_cast<int>(Z.size());
}

void hierarchy_inconsistent_flat(const double* Z, int n_Z, int depth, double** out, int* n_out) {
    if (n_Z % 4) throw std::invalid_argument("linkage matrix must be (n - 1) x 4");
    const std::vector<double> z(Z, Z + n_Z);
    const auto R = hierarchy_inconsistent(z, n_Z / 4 + 1, depth);
    *out = copy_out(R);
    *n_out = static_cast<int>(R.size());
}

void hierarchy_fcluster_flat(const double* Z, int n_Z, const std::string& criterion, double t, int depth,
                             const double* R, int n_R, const double* monocrit, int n_monocrit, int** out,
                             int* n_out) {
    if (n_Z % 4) throw std::invalid_argument("linkage matrix must be (n - 1) x 4");
    const std::vector<double> z(Z, Z + n_Z), r(R, R + n_R), mc(monocrit, monocrit + n_monocrit);
    const auto T = hierarchy_fcluster(z, n_Z / 4 + 1, criterion, t, depth, r, mc);
    *out = copy_out(T);
    *n_out = static_cast<int>(T.size());
}

}  // namespace tttrlib
