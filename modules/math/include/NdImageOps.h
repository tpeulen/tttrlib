// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_NDIMAGEOPS_H
#define TTTRLIB_NDIMAGEOPS_H
// NdImageOps.h -- the flat, array-in/array-out facade of NdImage.h that the
// language bindings wrap (the ImageOps.h pattern): every image travels as a
// flat C-order buffer plus a `shape` vector, every result leaves as a
// malloc'd buffer the binding hands to the caller. The scipy-shaped Python
// API (`tttrlib.ndimage`) is written over these; nothing here knows a dtype.

#include "NdImage.h"

#include <cstdint>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace tttrlib {
namespace ndimage_ops {

namespace detail {

template <typename T>
T* alloc(size_t n) {
    // malloc, not new[]: the binding frees ARGOUTVIEWM buffers with free()
    void* p = std::malloc(sizeof(T) * (n ? n : 1));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
}

inline ndimage::Shape to_shape(const long long* shape, int ndim) {
    if (ndim < 1) throw std::invalid_argument("ndimage: shape must have at least one axis");
    ndimage::Shape s(static_cast<size_t>(ndim));
    for (int i = 0; i < ndim; ++i) {
        if (shape[i] < 0) throw std::invalid_argument("ndimage: negative axis length");
        s[i] = static_cast<std::ptrdiff_t>(shape[i]);
    }
    return s;
}

inline void check_size(const ndimage::Shape& s, long long n, const char* what) {
    if (ndimage::shape_size(s) != n)
        throw std::invalid_argument(std::string("ndimage: ") + what + " size does not match its shape");
}

inline ndimage::Origins to_origins(const long long* origins, int n, int ndim) {
    if (n == 0) return ndimage::Origins(static_cast<size_t>(ndim), 0);
    if (n != ndim) throw std::invalid_argument("ndimage: one origin per axis");
    ndimage::Origins o(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) o[i] = static_cast<std::ptrdiff_t>(origins[i]);
    return o;
}

}  // namespace detail

/// correlate1d over a flat C-order array of shape `shape` (see NdImage.h).
inline void ndi_correlate1d(const double* src, int n_src, const long long* shape, int ndim, int axis,
                            const double* weights, int n_weights, int mode, double cval, int origin,
                            double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::correlate1d(src, s, axis, weights, n_weights, mode, cval, origin, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// uniform_filter1d (running box mean) along `axis`.
inline void ndi_uniform_filter1d(const double* src, int n_src, const long long* shape, int ndim, int axis,
                                 int size, int mode, double cval, int origin, double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::uniform_filter1d(src, s, axis, size, mode, cval, origin, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// Sliding minimum (`minimum != 0`) or maximum of `size` samples along `axis`.
inline void ndi_min_or_max_filter1d(const double* src, int n_src, const long long* shape, int ndim, int axis,
                                    int size, int mode, double cval, int origin, int minimum,
                                    double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::min_or_max_filter1d(src, s, axis, size, mode, cval, origin, minimum != 0, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// N-D correlate with flat `weights` of shape `wshape`.
inline void ndi_correlate(const double* src, int n_src, const long long* shape, int ndim,
                          const double* weights, int n_weights, const long long* wshape, int wndim,
                          int mode, double cval, const long long* origins, int n_origins,
                          double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    const auto ws = detail::to_shape(wshape, wndim);
    detail::check_size(s, n_src, "input");
    detail::check_size(ws, n_weights, "weights");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::correlate(src, s, weights, ws, mode, cval, detail::to_origins(origins, n_origins, ndim), out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// Minimum (`minimum != 0`) or maximum over a flat boolean footprint.
inline void ndi_min_or_max_filter(const double* src, int n_src, const long long* shape, int ndim,
                                  const unsigned char* footprint, int n_footprint, const long long* wshape,
                                  int wndim, int mode, double cval, const long long* origins, int n_origins,
                                  int minimum, double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    const auto fs = detail::to_shape(wshape, wndim);
    detail::check_size(s, n_src, "input");
    detail::check_size(fs, n_footprint, "footprint");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::min_or_max_filter(src, s, footprint, fs, mode, cval,
                                   detail::to_origins(origins, n_origins, ndim), minimum != 0, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// The `rank`-th smallest value over a flat boolean footprint.
inline void ndi_rank_filter(const double* src, int n_src, const long long* shape, int ndim,
                            const unsigned char* footprint, int n_footprint, const long long* wshape, int wndim,
                            int rank, int mode, double cval, const long long* origins, int n_origins,
                            double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    const auto fs = detail::to_shape(wshape, wndim);
    detail::check_size(s, n_src, "input");
    detail::check_size(fs, n_footprint, "footprint");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    try {
        ndimage::rank_filter(src, s, footprint, fs, rank, mode, cval,
                             detail::to_origins(origins, n_origins, ndim), out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// Connected-component labels (int64, 0 = background) and their count.
inline void ndi_label(const unsigned char* bsrc, int n_bsrc, const long long* shape, int ndim,
                      const unsigned char* footprint, int n_footprint, long long** ldst, int* ldst_n,
                      long long* n_found) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_bsrc, "input");
    long long expected = 1;
    for (int i = 0; i < ndim; ++i) expected *= 3;
    if (n_footprint != expected) throw std::invalid_argument("label: structure must be 3 along every axis");
    long long* out = detail::alloc<long long>(static_cast<size_t>(n_bsrc));
    try {
        static_assert(sizeof(long long) == sizeof(std::int64_t), "label buffer");
        *n_found = ndimage::label(bsrc, s, footprint, reinterpret_cast<std::int64_t*>(out));
    } catch (...) {
        std::free(out);
        throw;
    }
    *ldst = out;
    *ldst_n = n_bsrc;
}

/// Bounding boxes of labels 1..max_label, `2 * ndim` entries each, -1 if absent.
inline void ndi_find_objects(const long long* labels, int n_labels, const long long* shape, int ndim,
                             long long max_label, long long** ldst, int* ldst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_labels, "labels");
    const auto boxes = ndimage::find_objects(reinterpret_cast<const std::int64_t*>(labels), s, max_label);
    long long* out = detail::alloc<long long>(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) out[i] = boxes[i];
    *ldst = out;
    *ldst_n = static_cast<int>(boxes.size());
}

/// Binary erosion, or with `invert` the dilation scipy builds from it.
/// An empty `mask` means no mask. `changed` reports the last pass.
inline void ndi_binary_erosion(const unsigned char* bsrc, int n_bsrc, const long long* shape, int ndim,
                               const unsigned char* footprint, int n_footprint, const long long* wshape,
                               int wndim, const long long* origins, int n_origins, int border_value,
                               int invert, const unsigned char* mask, int n_mask, int iterations,
                               unsigned char** bdst, int* bdst_n, int* changed) {
    const auto s = detail::to_shape(shape, ndim);
    const auto fs = detail::to_shape(wshape, wndim);
    detail::check_size(s, n_bsrc, "input");
    detail::check_size(fs, n_footprint, "structure");
    if (n_mask != 0) detail::check_size(s, n_mask, "mask");
    unsigned char* out = detail::alloc<unsigned char>(static_cast<size_t>(n_bsrc));
    try {
        *changed = ndimage::binary_erosion(bsrc, s, footprint, fs, detail::to_origins(origins, n_origins, ndim),
                                           border_value, invert != 0, n_mask ? mask : nullptr, iterations, out)
                       ? 1
                       : 0;
    } catch (...) {
        std::free(out);
        throw;
    }
    *bdst = out;
    *bdst_n = n_bsrc;
}

/// Exact Euclidean feature transform; `ndim * size` int32 coordinates,
/// component-major. An empty `sampling` means unit spacing.
inline void ndi_euclidean_feature_transform(const unsigned char* bsrc, int n_bsrc, const long long* shape,
                                            int ndim, const double* sampling, int n_sampling, int** idst,
                                            int* idst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_bsrc, "input");
    if (n_sampling != 0 && n_sampling != ndim)
        throw std::invalid_argument("distance_transform_edt: one sampling value per axis");
    const size_t n = static_cast<size_t>(n_bsrc) * static_cast<size_t>(ndim);
    int* out = detail::alloc<int>(n);
    static_assert(sizeof(int) == sizeof(std::int32_t), "feature buffer");
    try {
        ndimage::euclidean_feature_transform(bsrc, s, n_sampling ? sampling : nullptr,
                                             reinterpret_cast<std::int32_t*>(out));
    } catch (...) {
        std::free(out);
        throw;
    }
    *idst = out;
    *idst_n = static_cast<int>(n);
}

/// B-spline prefilter along `axis`, returned as a new array.
inline void ndi_spline_filter1d(const double* src, int n_src, const long long* shape, int ndim, int axis,
                                int order, int mode, double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    double* out = detail::alloc<double>(static_cast<size_t>(n_src));
    for (int i = 0; i < n_src; ++i) out[i] = src[i];
    try {
        ndimage::spline_filter1d(out, s, axis, order, mode);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_src;
}

/// Sample a prefiltered array at `n_points` coordinates (`ndim * n_points`, axis-major).
inline void ndi_map_coordinates(const double* src, int n_src, const long long* shape, int ndim,
                                const double* coords, int n_coords, int order, int mode, double cval,
                                int nprepad, double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    if (n_coords % ndim) throw std::invalid_argument("map_coordinates: coordinates must be ndim x n");
    const int n_points = n_coords / ndim;
    double* out = detail::alloc<double>(static_cast<size_t>(n_points));
    try {
        ndimage::map_coordinates(src, s, coords, n_points, order, mode, cval, nprepad, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = n_points;
}

/// `scipy.ndimage.shift` of a prefiltered (and `nprepad`-padded) array.
inline void ndi_shift(const double* src, int n_src, const long long* shape, int ndim, const double* shifts,
                      int n_shifts, int order, int mode, double cval, int nprepad, double** dst, int* dst_n) {
    const auto s = detail::to_shape(shape, ndim);
    detail::check_size(s, n_src, "input");
    if (n_shifts != ndim) throw std::invalid_argument("shift: one shift per axis");
    long long n_out = 1;
    for (int i = 0; i < ndim; ++i) n_out *= (shape[i] - 2LL * nprepad);
    if (n_out < 0) n_out = 0;
    double* out = detail::alloc<double>(static_cast<size_t>(n_out));
    try {
        ndimage::shift(src, s, shifts, order, mode, cval, nprepad, out);
    } catch (...) {
        std::free(out);
        throw;
    }
    *dst = out;
    *dst_n = static_cast<int>(n_out);
}

}  // namespace ndimage_ops
}  // namespace tttrlib

#endif  // TTTRLIB_NDIMAGEOPS_H
