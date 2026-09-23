// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_WARPAFFINE_H
#define TTTRLIB_WARPAFFINE_H

// Validation: A/B-TESTED 2026-08-28 -- vs a per-pixel evaluation of the same
//   inverse-mapped bilinear sample on random images for identity, pure
//   translation, rotation and scale matrices (1e-12 relative); the
//   singular-matrix refusal and nearest-pixel degenerate path in
//   test/cpp/test_gradients_warp.cpp.

// Affine geometric correction: `dst(x, y) = src(m^-1 (x, y))`, bilinear,
// replicated border. Covers rotation, skew, anisotropic scale and
// translation in one matrix -- everything short of the perspective term a
// scan-stage correction or a camera-to-scan registration needs.
//
// Concept lineage: the 2x3 row-major forward matrix with an internally
// inverted inverse map (computed once, not per pixel) and the replicated
// border follow ermig1979/Simd's WarpAffine (okf/design/simd-port-survey.md),
// re-expressed std-only and type-generic; the original is MIT,
// (c) 2011-2026 Yermalayeu Ihar, https://github.com/ermig1979/Simd.
//
// `matrix` is the FORWARD transform in source coordinates, OpenCV's
// `warpAffine` convention: to place src content at dst position p, the map
// must send it there, and this kernel samples through its inverse. `src` and
// `dst` must not alias.

#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace tttrlib {

namespace warp_affine {

namespace detail {

/// Invert a 2x3 row-major affine map; false when singular.
inline bool invert(const double m[6], double inv[6]) {
    const double det = m[0] * m[4] - m[1] * m[3];
    if (det == 0.0) return false;
    const double id = 1.0 / det;
    inv[0] = m[4] * id;
    inv[1] = -m[1] * id;
    inv[2] = (m[1] * m[5] - m[4] * m[2]) * id;
    inv[3] = -m[3] * id;
    inv[4] = m[0] * id;
    inv[5] = (m[3] * m[2] - m[0] * m[5]) * id;
    return true;
}

}  // namespace detail

/// Bilinear warp of `src` (rows x cols) into `dst` (same extent) through the
/// inverse of `matrix` (forward map, 2x3 row-major:
/// `[m00 m01 m02; m10 m11 m12]`). Border replicates. Throws on a singular
/// matrix rather than emitting a garbage image.
template <class T>
inline void warp_affine(const T* src, T* dst, int rows, int cols, const double matrix[6]) {
    if (rows <= 0 || cols <= 0) throw std::runtime_error("warp_affine: empty image");
    double inv[6];
    if (!detail::invert(matrix, inv))
        throw std::runtime_error("warp_affine: singular matrix");
#if defined(_OPENMP)
    #pragma omp parallel for schedule(static) if(rows >= 128)
#endif
    for (int y = 0; y < rows; ++y) {
        // the affine inverse applied to the row origin, stepped per pixel
        double sx = inv[0] * 0.0 + inv[1] * y + inv[2];
        double sy = inv[3] * 0.0 + inv[4] * y + inv[5];
        T* drow = dst + static_cast<std::ptrdiff_t>(y) * cols;
        for (int x = 0; x < cols; ++x, sx += inv[0], sy += inv[3]) {
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            const double fx = sx - x0;
            const double fy = sy - y0;
            // clamp both taps: replicate border, and keep fx/fy usable when
            // the sample lies outside (nearest pixel carries the weight)
            int x1 = x0 + 1, y1 = y0 + 1;
            const bool out = x0 < -1 || y0 < -1 || x0 > cols || y0 > rows;
            if (out) {  // far outside: nearest corner pixel, no interpolation
                x0 = x0 < 0 ? 0 : (x0 > cols - 1 ? cols - 1 : x0);
                y0 = y0 < 0 ? 0 : (y0 > rows - 1 ? rows - 1 : y0);
                drow[x] = src[static_cast<std::ptrdiff_t>(y0) * cols + x0];
                continue;
            }
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x0 > cols - 1) x0 = cols - 1;
            if (y0 > rows - 1) y0 = rows - 1;
            if (x1 > cols - 1) x1 = cols - 1;
            if (y1 > rows - 1) y1 = rows - 1;
            const T* p00 = src + static_cast<std::ptrdiff_t>(y0) * cols + x0;
            const T* p01 = src + static_cast<std::ptrdiff_t>(y0) * cols + x1;
            const T* p10 = src + static_cast<std::ptrdiff_t>(y1) * cols + x0;
            const T* p11 = src + static_cast<std::ptrdiff_t>(y1) * cols + x1;
            const double top = static_cast<double>(*p00) * (1.0 - fx) +
                               static_cast<double>(*p01) * fx;
            const double bot = static_cast<double>(*p10) * (1.0 - fx) +
                               static_cast<double>(*p11) * fx;
            drow[x] = static_cast<T>(top * (1.0 - fy) + bot * fy);
        }
    }
}

}  // namespace warp_affine

}  // namespace tttrlib

#endif  // TTTRLIB_WARPAFFINE_H
