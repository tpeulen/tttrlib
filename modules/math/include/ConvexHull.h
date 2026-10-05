// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_CONVEXHULL_H
#define TTTRLIB_CONVEXHULL_H
// Validation: A/B-TESTED 2026-10-05 -- vs scipy.spatial.ConvexHull (qhull) on
//   random clouds, pixel-corner sets with collinear runs and duplicates: the
//   same vertex set, areas and perimeters to 1e-12, equations (unit outward
//   normals and offsets) to 1e-12 after matching facets
//   (test/python/misc/test_ndimage_parity.py).
//
// ConvexHull.h -- the 2-D convex hull, by Andrew's monotone chain.
//
// What image analysis asks of a hull is the plane case only (region
// solidity, Feret diameters, filled convex images), and for that qhull is a
// dependency in search of a problem: sort, two passes, done. The output is
// shaped like scipy's so callers do not change: vertex indices in
// counterclockwise order with collinear points dropped (qhull's default
// `Qt` triangulated output keeps only extreme points too), and one
// `[nx, ny, offset]` row per edge with a unit outward normal, so that
// `n . x + offset <= 0` holds for every point inside.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace tttrlib {

struct ConvexHull2D {
    std::vector<int> vertices;      ///< indices into the input, counterclockwise
    std::vector<double> equations;  ///< 3 per edge: nx, ny, offset
    double area = 0.0;              ///< enclosed area (scipy's `volume` in 2-D)
    double perimeter = 0.0;         ///< boundary length (scipy's `area` in 2-D)
};

/// Hull of `n` points stored row-major as `(x, y)` pairs. Throws
/// `std::invalid_argument` when fewer than three non-collinear points exist,
/// where qhull raises QhullError.
inline ConvexHull2D convex_hull_2d(const double* xy, int n) {
    if (n < 3) throw std::invalid_argument("convex_hull_2d: need at least 3 points");
    std::vector<int> idx(static_cast<size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    auto X = [&](int i) { return xy[2 * i]; };
    auto Y = [&](int i) { return xy[2 * i + 1]; };
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return X(a) < X(b) || (X(a) == X(b) && (Y(a) < Y(b) || (Y(a) == Y(b) && a < b)));
    });
    auto cross = [&](int o, int a, int b) {
        return (X(a) - X(o)) * (Y(b) - Y(o)) - (Y(a) - Y(o)) * (X(b) - X(o));
    };
    std::vector<int> h(static_cast<size_t>(2 * n));
    int k = 0;
    for (int i = 0; i < n; ++i) {  // lower chain
        while (k >= 2 && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k;
        h[k++] = idx[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {  // upper chain
        while (k >= t && cross(h[k - 2], h[k - 1], idx[i]) <= 0) --k;
        h[k++] = idx[i];
    }
    h.resize(static_cast<size_t>(k - 1));  // last point repeats the first
    if (h.size() < 3) throw std::invalid_argument("convex_hull_2d: points are collinear");
    ConvexHull2D out;
    out.vertices = h;
    const size_t m = h.size();
    for (size_t i = 0; i < m; ++i) {
        const int a = h[i], b = h[(i + 1) % m];
        const double ex = X(b) - X(a), ey = Y(b) - Y(a);
        const double len = std::hypot(ex, ey);
        // counterclockwise boundary: the outward normal points right of the edge
        const double nx = ey / len, ny = -ex / len;
        out.equations.push_back(nx);
        out.equations.push_back(ny);
        out.equations.push_back(-(nx * X(a) + ny * Y(a)));
        out.area += X(a) * Y(b) - X(b) * Y(a);
        out.perimeter += len;
    }
    out.area *= 0.5;
    return out;
}

/// Binding facade: vertices, then equations, area and perimeter, in malloc'd buffers.
inline void convex_hull_2d_flat(const double* xy, int n_rows, int n_cols, long long** out_vertices,
                                int* n_out_vertices, double** out_equations, int* n_out_equations,
                                double* area, double* perimeter) {
    if (n_cols != 2) throw std::invalid_argument("convex_hull_2d: points must be n x 2");
    const ConvexHull2D hull = convex_hull_2d(xy, n_rows);
    auto* v = static_cast<long long*>(std::malloc(sizeof(long long) * hull.vertices.size()));
    auto* e = static_cast<double*>(std::malloc(sizeof(double) * hull.equations.size()));
    if (!v || !e) {
        std::free(v);
        std::free(e);
        throw std::bad_alloc();
    }
    std::copy(hull.vertices.begin(), hull.vertices.end(), v);
    std::copy(hull.equations.begin(), hull.equations.end(), e);
    *out_vertices = v;
    *n_out_vertices = static_cast<int>(hull.vertices.size());
    *out_equations = e;
    *n_out_equations = static_cast<int>(hull.equations.size());
    *area = hull.area;
    *perimeter = hull.perimeter;
}

}  // namespace tttrlib

#endif  // TTTRLIB_CONVEXHULL_H
