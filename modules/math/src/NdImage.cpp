// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from scipy/ndimage/src (ni_filters.c, ni_support.c, ni_morphology.c,
// ni_interpolation.c, ni_splines.c) and scipy/ndimage/src/_ni_label.pyx.
// Copyright (C) 2003-2005 Peter J. Verveer; Copyright the SciPy developers.
// BSD-3-Clause, the same license as this file. See NdImage.h for what was
// kept verbatim and why.

// load-bearing, and the opposite of Cluster.cpp's pragma: scipy's wheels are
// built with clang's default contraction, so `acc += a * w` is an fma there.
// Measured against scipy 1.18.0 on arm64: with contraction OFF every
// filter, the spline prefilter and the sampler differ by 1-8 ulp; ON, all
// 576 ndimage checks in test_ndimage_parity.py are bit-identical.
#pragma STDC FP_CONTRACT ON

#include "NdImage.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <stdexcept>
#include <string>

namespace tttrlib {
namespace ndimage {

namespace {

std::vector<std::ptrdiff_t> c_strides(const Shape& shape) {
    std::vector<std::ptrdiff_t> s(shape.size(), 1);
    for (int a = static_cast<int>(shape.size()) - 2; a >= 0; --a) s[a] = s[a + 1] * shape[a + 1];
    return s;
}

void check_shape(const Shape& shape) {
    if (shape.empty()) throw std::invalid_argument("ndimage: arrays must have at least one axis");
    for (auto n : shape)
        if (n < 0) throw std::invalid_argument("ndimage: negative axis length");
}

void check_axis(const Shape& shape, int axis) {
    if (axis < 0 || axis >= static_cast<int>(shape.size()))
        throw std::invalid_argument("ndimage: axis out of range");
}

void check_mode(int mode) {
    if (mode < kNearest || mode > kGridConstant)
        throw std::invalid_argument("ndimage: boundary mode not supported");
}

/// Offsets (in elements) of the first element of every line along `axis`.
std::vector<std::ptrdiff_t> line_starts(const Shape& shape, int axis) {
    const auto strides = c_strides(shape);
    const std::ptrdiff_t len = shape[axis];
    const std::ptrdiff_t n_lines = len > 0 ? shape_size(shape) / len : 0;
    std::vector<std::ptrdiff_t> starts;
    starts.reserve(static_cast<size_t>(n_lines));
    const int nd = static_cast<int>(shape.size());
    std::vector<std::ptrdiff_t> idx(nd, 0);
    for (std::ptrdiff_t l = 0; l < n_lines; ++l) {
        std::ptrdiff_t off = 0;
        for (int a = 0; a < nd; ++a) off += idx[a] * strides[a];
        starts.push_back(off);
        for (int a = nd - 1; a >= 0; --a) {
            if (a == axis) continue;
            if (++idx[a] < shape[a]) break;
            idx[a] = 0;
        }
    }
    return starts;
}

/// One true element of a footprint: its per-axis offset from the origin,
/// and the flat offset that offset is in the interior of the array.
struct Footprint {
    std::vector<std::ptrdiff_t> rel;   ///< n_elements * ndim
    std::vector<std::ptrdiff_t> flat;  ///< n_elements
    std::vector<std::ptrdiff_t> lo;    ///< first interior coordinate per axis
    std::vector<std::ptrdiff_t> hi;    ///< one past the last
    std::ptrdiff_t n = 0;
};

Footprint make_footprint(const Shape& shape, const Shape& fshape, const Origins& origins,
                         const std::vector<bool>& keep) {
    const int nd = static_cast<int>(shape.size());
    if (static_cast<int>(fshape.size()) != nd)
        throw std::invalid_argument("ndimage: footprint rank must match the input rank");
    if (!origins.empty() && static_cast<int>(origins.size()) != nd)
        throw std::invalid_argument("ndimage: one origin per axis");
    std::vector<std::ptrdiff_t> orgn(nd);
    for (int a = 0; a < nd; ++a) {
        const std::ptrdiff_t o = origins.empty() ? 0 : origins[a];
        orgn[a] = fshape[a] / 2 + o;
        if (fshape[a] < 1) throw std::invalid_argument("ndimage: empty footprint axis");
        if (orgn[a] < 0 || orgn[a] >= fshape[a]) throw std::invalid_argument("ndimage: invalid origin");
    }
    const auto strides = c_strides(shape);
    Footprint fp;
    fp.lo.resize(nd);
    fp.hi.resize(nd);
    for (int a = 0; a < nd; ++a) {
        fp.lo[a] = orgn[a];
        fp.hi[a] = shape[a] - fshape[a] + orgn[a] + 1;
    }
    const std::ptrdiff_t fsize = shape_size(fshape);
    std::vector<std::ptrdiff_t> c(nd, 0);
    for (std::ptrdiff_t k = 0; k < fsize; ++k) {
        if (keep[static_cast<size_t>(k)]) {
            std::ptrdiff_t flat = 0;
            for (int a = 0; a < nd; ++a) {
                const std::ptrdiff_t r = c[a] - orgn[a];
                fp.rel.push_back(r);
                flat += r * strides[a];
            }
            fp.flat.push_back(flat);
            ++fp.n;
        }
        for (int a = nd - 1; a >= 0; --a) {
            if (++c[a] < fshape[a]) break;
            c[a] = 0;
        }
    }
    return fp;
}

/// Visit every element in C order with its coordinates; `fn(flat, coords, interior)`.
template <class F>
void for_each_point(const Shape& shape, const Footprint& fp, F fn) {
    const int nd = static_cast<int>(shape.size());
    const std::ptrdiff_t size = shape_size(shape);
    std::vector<std::ptrdiff_t> p(nd, 0);
    for (std::ptrdiff_t flat = 0; flat < size; ++flat) {
        bool interior = true;
        for (int a = 0; a < nd; ++a)
            if (p[a] < fp.lo[a] || p[a] >= fp.hi[a]) { interior = false; break; }
        fn(flat, p, interior);
        for (int a = nd - 1; a >= 0; --a) {
            if (++p[a] < shape[a]) break;
            p[a] = 0;
        }
    }
}

/// Flat offset of footprint element `k` around `p`, or -1 when it reads `cval`.
std::ptrdiff_t element_offset(const Shape& shape, const std::vector<std::ptrdiff_t>& strides,
                              const Footprint& fp, std::ptrdiff_t k,
                              const std::vector<std::ptrdiff_t>& p, int mode) {
    const int nd = static_cast<int>(shape.size());
    std::ptrdiff_t off = 0;
    for (int a = 0; a < nd; ++a) {
        bool outside = false;
        const std::ptrdiff_t cc = map_filter_index(p[a] + fp.rel[static_cast<size_t>(k * nd + a)],
                                                   shape[a], mode, outside);
        if (outside) return -1;
        off += cc * strides[a];
    }
    return off;
}

}  // namespace

std::ptrdiff_t shape_size(const Shape& shape) {
    std::ptrdiff_t n = 1;
    for (auto s : shape) n *= s;
    return n;
}

void extend_line(double* buffer, std::ptrdiff_t line_length, std::ptrdiff_t size_before,
                 std::ptrdiff_t size_after, int mode, double cval) {
    double* first = buffer + size_before;
    double* last = first + line_length;
    double *src, *dst, val;
    if (line_length == 1 && mode == kMirror) mode = kNearest;
    switch (mode) {
        case kNearest:
            src = first;
            dst = buffer;
            val = *src;
            while (size_before--) *dst++ = val;
            src = last - 1;
            dst = last;
            val = *src;
            while (size_after--) *dst++ = val;
            break;
        case kWrap:
        case kGridWrap:
            src = last - 1;
            dst = first - 1;
            while (size_before--) *dst-- = *src--;
            src = first;
            dst = last;
            while (size_after--) *dst++ = *src++;
            break;
        case kReflect:
            src = first;
            dst = first - 1;
            while (size_before && src < last) {
                *dst-- = *src++;
                --size_before;
            }
            src = last - 1;
            while (size_before--) *dst-- = *src--;
            src = last - 1;
            dst = last;
            while (size_after && src >= first) {
                *dst++ = *src--;
                --size_after;
            }
            src = first;
            while (size_after--) *dst++ = *src++;
            break;
        case kMirror:
            src = first + 1;
            dst = first - 1;
            while (size_before && src < last) {
                *dst-- = *src++;
                --size_before;
            }
            src = last - 2;
            while (size_before--) *dst-- = *src--;
            src = last - 2;
            dst = last;
            while (size_after && src >= first) {
                *dst++ = *src--;
                --size_after;
            }
            src = first + 1;
            while (size_after--) *dst++ = *src++;
            break;
        case kConstant:
        case kGridConstant:
            dst = buffer;
            while (size_before--) *dst++ = cval;
            dst = last;
            while (size_after--) *dst++ = cval;
            break;
        default:
            throw std::invalid_argument("ndimage: boundary mode not supported");
    }
}

std::ptrdiff_t map_filter_index(std::ptrdiff_t cc, std::ptrdiff_t len, int mode, bool& outside) {
    outside = false;
    switch (mode) {
        case kMirror:
            if (cc < 0) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz2 = static_cast<int>(2 * len - 2);
                    cc = static_cast<std::ptrdiff_t>(sz2) * static_cast<int>(-cc / sz2) + cc;
                    cc = cc <= 1 - len ? cc + sz2 : -cc;
                }
            } else if (cc >= len) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz2 = static_cast<int>(2 * len - 2);
                    cc -= static_cast<std::ptrdiff_t>(sz2) * static_cast<int>(cc / sz2);
                    if (cc >= len) cc = sz2 - cc;
                }
            }
            break;
        case kReflect:
            if (cc < 0) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz2 = static_cast<int>(2 * len);
                    if (cc < -sz2) cc = static_cast<std::ptrdiff_t>(sz2) * static_cast<int>(-cc / sz2) + cc;
                    cc = cc < -len ? cc + sz2 : -cc - 1;
                }
            } else if (cc >= len) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz2 = static_cast<int>(2 * len);
                    cc -= static_cast<std::ptrdiff_t>(sz2) * static_cast<int>(cc / sz2);
                    if (cc >= len) cc = sz2 - cc - 1;
                }
            }
            break;
        case kWrap:
        case kGridWrap:
            if (cc < 0) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz = static_cast<int>(len);
                    cc += static_cast<std::ptrdiff_t>(sz) * static_cast<int>(-cc / sz);
                    if (cc < 0) cc += sz;
                }
            } else if (cc >= len) {
                if (len <= 1) {
                    cc = 0;
                } else {
                    const int sz = static_cast<int>(len);
                    cc -= static_cast<std::ptrdiff_t>(sz) * static_cast<int>(cc / sz);
                }
            }
            break;
        case kNearest:
            if (cc < 0) cc = 0;
            else if (cc >= len) cc = len - 1;
            break;
        case kConstant:
        case kGridConstant:
            if (cc < 0 || cc >= len) outside = true;
            break;
        default:
            throw std::invalid_argument("ndimage: boundary mode not supported");
    }
    return cc;
}

// ---------------------------------------------------------------------------
// separable filters
// ---------------------------------------------------------------------------

namespace {

/// Run `kernel(buffer_line, out_line)` over every line along `axis`, the
/// input line padded by `before`/`after` and extended by `mode`.
template <class K>
void run_lines(const double* in, const Shape& shape, int axis, std::ptrdiff_t before,
               std::ptrdiff_t after, int mode, double cval, double* out, K kernel) {
    check_shape(shape);
    check_axis(shape, axis);
    check_mode(mode);
    const std::ptrdiff_t len = shape[axis];
    if (len == 0 || shape_size(shape) == 0) return;
    const std::ptrdiff_t stride = c_strides(shape)[axis];
    std::vector<double> buf(static_cast<size_t>(len + before + after));
    std::vector<double> oline(static_cast<size_t>(len));
    for (const std::ptrdiff_t start : line_starts(shape, axis)) {
        for (std::ptrdiff_t i = 0; i < len; ++i) buf[before + i] = in[start + i * stride];
        extend_line(buf.data(), len, before, after, mode, cval);
        kernel(buf.data(), oline.data(), len);
        for (std::ptrdiff_t i = 0; i < len; ++i) out[start + i * stride] = oline[i];
    }
}

}  // namespace

void correlate1d(const double* in, const Shape& shape, int axis, const double* weights,
                 std::ptrdiff_t n_weights, int mode, double cval, std::ptrdiff_t origin,
                 double* out) {
    if (n_weights < 1) throw std::invalid_argument("correlate1d: no filter weights given");
    const std::ptrdiff_t size1 = n_weights / 2;
    const std::ptrdiff_t size2 = n_weights - size1 - 1;
    if (size1 + origin < 0 || size2 - origin < 0)
        throw std::invalid_argument("correlate1d: invalid origin");
    int symmetric = 0;
    if (n_weights & 1) {
        symmetric = 1;
        for (std::ptrdiff_t ii = 1; ii <= n_weights / 2; ++ii) {
            if (std::fabs(weights[ii + size1] - weights[size1 - ii]) > DBL_EPSILON) {
                symmetric = 0;
                break;
            }
        }
        if (symmetric == 0) {
            symmetric = -1;
            for (std::ptrdiff_t ii = 1; ii <= n_weights / 2; ++ii) {
                if (std::fabs(weights[size1 + ii] + weights[size1 - ii]) > DBL_EPSILON) {
                    symmetric = 0;
                    break;
                }
            }
        }
    }
    const double* fw = weights + size1;
    run_lines(in, shape, axis, size1 + origin, size2 - origin, mode, cval, out,
              [&](const double* buffer, double* oline, std::ptrdiff_t length) {
                  // the line buffer starts size1+origin early; scipy points iline at +size1
                  const double* iline = buffer + size1;
                  if (symmetric > 0) {
                      for (std::ptrdiff_t ll = 0; ll < length; ++ll) {
                          oline[ll] = iline[0] * fw[0];
                          for (std::ptrdiff_t jj = -size1; jj < 0; ++jj)
                              oline[ll] += (iline[jj] + iline[-jj]) * fw[jj];
                          ++iline;
                      }
                  } else if (symmetric < 0) {
                      for (std::ptrdiff_t ll = 0; ll < length; ++ll) {
                          oline[ll] = iline[0] * fw[0];
                          for (std::ptrdiff_t jj = -size1; jj < 0; ++jj)
                              oline[ll] += (iline[jj] - iline[-jj]) * fw[jj];
                          ++iline;
                      }
                  } else {
                      for (std::ptrdiff_t ll = 0; ll < length; ++ll) {
                          oline[ll] = iline[size2] * fw[size2];
                          for (std::ptrdiff_t jj = -size1; jj < size2; ++jj)
                              oline[ll] += iline[jj] * fw[jj];
                          ++iline;
                      }
                  }
              });
}

void uniform_filter1d(const double* in, const Shape& shape, int axis, std::ptrdiff_t size,
                      int mode, double cval, std::ptrdiff_t origin, double* out) {
    if (size < 1) throw std::invalid_argument("uniform_filter1d: incorrect filter size");
    const std::ptrdiff_t size1 = size / 2;
    const std::ptrdiff_t size2 = size - size1 - 1;
    if (size1 + origin < 0 || size2 - origin < 0)
        throw std::invalid_argument("uniform_filter1d: invalid origin");
    run_lines(in, shape, axis, size1 + origin, size2 - origin, mode, cval, out,
              [&](const double* iline, double* oline, std::ptrdiff_t length) {
                  double tmp = 0.0;
                  const double* l1 = iline;
                  const double* l2 = iline + size;
                  for (std::ptrdiff_t ll = 0; ll < size; ++ll) tmp += iline[ll];
                  oline[0] = tmp / static_cast<double>(size);
                  for (std::ptrdiff_t ll = 1; ll < length; ++ll) {
                      tmp += *l2++ - *l1++;
                      oline[ll] = tmp / static_cast<double>(size);
                  }
              });
}

void min_or_max_filter1d(const double* in, const Shape& shape, int axis, std::ptrdiff_t size,
                         int mode, double cval, std::ptrdiff_t origin, bool minimum,
                         double* out) {
    if (size < 1) throw std::invalid_argument("min_or_max_filter1d: incorrect filter size");
    const std::ptrdiff_t size1 = size / 2;
    const std::ptrdiff_t size2 = size - size1 - 1;
    if (size1 + origin < 0 || size2 - origin < 0)
        throw std::invalid_argument("min_or_max_filter1d: invalid origin");
    struct Pair {
        double value;
        std::ptrdiff_t death;
    };
    std::vector<Pair> ring(static_cast<size_t>(size));
    Pair* const ring0 = ring.data();
    Pair* const end = ring0 + size;
    run_lines(in, shape, axis, size1 + origin, size2 - origin, mode, cval, out,
              [&](const double* iline, double* oline, std::ptrdiff_t length) {
                  if (size == 1) {
                      std::copy(iline, iline + length, oline);
                      return;
                  }
                  // Richard Harter's ascending minima, as in NI_MinOrMaxFilter1D
                  Pair* minpair = ring0;
                  minpair->value = *iline++;
                  minpair->death = size;
                  Pair* last = ring0;
                  for (std::ptrdiff_t ll = 1; ll < size + length - 1; ++ll) {
                      const double val = *iline++;
                      if (minpair->death == ll) {
                          if (++minpair >= end) minpair = ring0;
                      }
                      if ((minimum && val <= minpair->value) || (!minimum && val >= minpair->value)) {
                          minpair->value = val;
                          minpair->death = ll + size;
                          last = minpair;
                      } else {
                          while ((minimum && last->value >= val) || (!minimum && last->value <= val)) {
                              if (last == ring0) last = end;
                              --last;
                          }
                          if (++last >= end) last = ring0;
                          last->value = val;
                          last->death = ll + size;
                      }
                      if (ll >= size - 1) *oline++ = minpair->value;
                  }
              });
}

std::vector<double> gaussian_kernel1d(double sigma, int order, std::ptrdiff_t radius) {
    if (order < 0) throw std::invalid_argument("gaussian_kernel1d: order must be non-negative");
    if (radius < 0) throw std::invalid_argument("gaussian_kernel1d: radius must be non-negative");
    const std::ptrdiff_t n = 2 * radius + 1;
    const double sigma2 = sigma * sigma;
    std::vector<double> phi(static_cast<size_t>(n));
    double sum = 0.0;
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i - radius);
        phi[i] = std::exp(-0.5 / sigma2 * (x * x));
        sum += phi[i];
    }
    for (auto& v : phi) v /= sum;
    if (order > 0) {
        // f(x) = q(x) * phi(x); q' + q * p' applied `order` times, p' = -x / sigma^2
        std::vector<double> q(static_cast<size_t>(order + 1), 0.0), qn(q.size());
        q[0] = 1.0;
        for (int it = 0; it < order; ++it) {
            std::fill(qn.begin(), qn.end(), 0.0);
            for (int e = 0; e <= order; ++e) {
                if (e + 1 <= order) qn[e] += (e + 1) * q[e + 1];   // derivative
                if (e >= 1) qn[e] += q[e - 1] / -sigma2;           // times p'(x)
            }
            q.swap(qn);
        }
        for (std::ptrdiff_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(i - radius);
            double poly = 0.0, xp = 1.0;
            for (int e = 0; e <= order; ++e) {
                poly += xp * q[e];
                xp *= x;
            }
            phi[i] *= poly;
        }
    }
    std::reverse(phi.begin(), phi.end());
    return phi;
}

// ---------------------------------------------------------------------------
// N-D footprint filters
// ---------------------------------------------------------------------------

void correlate(const double* in, const Shape& shape, const double* weights, const Shape& wshape,
               int mode, double cval, const Origins& origins, double* out) {
    check_shape(shape);
    check_mode(mode);
    const std::ptrdiff_t wsize = shape_size(wshape);
    std::vector<bool> keep(static_cast<size_t>(wsize));
    std::vector<double> ww;
    for (std::ptrdiff_t k = 0; k < wsize; ++k) {
        keep[k] = std::fabs(weights[k]) > DBL_EPSILON;
        if (keep[k]) ww.push_back(weights[k]);
    }
    const Footprint fp = make_footprint(shape, wshape, origins, keep);
    const auto strides = c_strides(shape);
    for_each_point(shape, fp, [&](std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p, bool interior) {
        double tmp = 0.0;
        if (interior) {
            for (std::ptrdiff_t k = 0; k < fp.n; ++k) tmp += ww[k] * in[flat + fp.flat[k]];
        } else {
            for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
                const std::ptrdiff_t off = element_offset(shape, strides, fp, k, p, mode);
                tmp += ww[k] * (off < 0 ? cval : in[off]);
            }
        }
        out[flat] = tmp;
    });
}

void min_or_max_filter(const double* in, const Shape& shape, const std::uint8_t* footprint,
                       const Shape& fshape, int mode, double cval, const Origins& origins,
                       bool minimum, double* out) {
    check_shape(shape);
    check_mode(mode);
    const std::ptrdiff_t fsize = shape_size(fshape);
    std::vector<bool> keep(static_cast<size_t>(fsize));
    bool any = false;
    for (std::ptrdiff_t k = 0; k < fsize; ++k) any |= (keep[k] = footprint[k] != 0);
    if (!any) throw std::invalid_argument("min_or_max_filter: all-zero footprint is not supported");
    const Footprint fp = make_footprint(shape, fshape, origins, keep);
    const auto strides = c_strides(shape);
    for_each_point(shape, fp, [&](std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p, bool interior) {
        auto value = [&](std::ptrdiff_t k) {
            if (interior) return in[flat + fp.flat[k]];
            const std::ptrdiff_t off = element_offset(shape, strides, fp, k, p, mode);
            return off < 0 ? cval : in[off];
        };
        double res = value(0);
        for (std::ptrdiff_t k = 1; k < fp.n; ++k) {
            const double tmp = value(k);
            if (minimum) {
                if (tmp < res) res = tmp;
            } else {
                if (tmp > res) res = tmp;
            }
        }
        out[flat] = res;
    });
}

namespace {

double ni_select(double* buffer, std::ptrdiff_t min, std::ptrdiff_t max, std::ptrdiff_t rank) {
    // NI_Select verbatim (Hoare partition on the first element)
    for (;;) {
        if (min == max) return buffer[min];
        const double x = buffer[min];
        std::ptrdiff_t ii = min - 1;
        std::ptrdiff_t jj = max + 1;
        for (;;) {
            do jj--;
            while (buffer[jj] > x);
            do ii++;
            while (buffer[ii] < x);
            if (ii < jj) {
                const double t = buffer[ii];
                buffer[ii] = buffer[jj];
                buffer[jj] = t;
            } else {
                break;
            }
        }
        ii = jj - min + 1;
        if (rank < ii) {
            max = jj;
        } else {
            min = jj + 1;
            rank -= ii;
        }
    }
}

}  // namespace

void rank_filter(const double* in, const Shape& shape, const std::uint8_t* footprint,
                 const Shape& fshape, std::ptrdiff_t rank, int mode, double cval,
                 const Origins& origins, double* out) {
    check_shape(shape);
    check_mode(mode);
    const std::ptrdiff_t fsize = shape_size(fshape);
    std::vector<bool> keep(static_cast<size_t>(fsize));
    for (std::ptrdiff_t k = 0; k < fsize; ++k) keep[k] = footprint[k] != 0;
    const Footprint fp = make_footprint(shape, fshape, origins, keep);
    if (rank < 0 || rank >= fp.n) throw std::invalid_argument("rank_filter: rank not within filter footprint size");
    const auto strides = c_strides(shape);
    std::vector<double> buffer(static_cast<size_t>(fp.n));
    for_each_point(shape, fp, [&](std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p, bool interior) {
        for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
            if (interior) {
                buffer[k] = in[flat + fp.flat[k]];
            } else {
                const std::ptrdiff_t off = element_offset(shape, strides, fp, k, p, mode);
                buffer[k] = off < 0 ? cval : in[off];
            }
        }
        out[flat] = ni_select(buffer.data(), 0, fp.n - 1, rank);
    });
}

// ---------------------------------------------------------------------------
// labelling
// ---------------------------------------------------------------------------

std::int64_t label(const std::uint8_t* in, const Shape& shape, const std::uint8_t* structure,
                   std::int64_t* out) {
    check_shape(shape);
    const int nd = static_cast<int>(shape.size());
    const std::ptrdiff_t size = shape_size(shape);
    Shape sshape(nd, 3);
    const std::ptrdiff_t ssize = shape_size(sshape);
    for (std::ptrdiff_t k = 0; k < ssize; ++k)
        if ((structure[k] != 0) != (structure[ssize - 1 - k] != 0))
            throw std::invalid_argument("label: structuring element is not symmetric");
    // only the neighbours that come earlier in a C-order scan: the first half
    std::vector<bool> keep(static_cast<size_t>(ssize), false);
    for (std::ptrdiff_t k = 0; k < ssize / 2; ++k) keep[k] = structure[k] != 0;
    const Footprint fp = make_footprint(shape, sshape, Origins{}, keep);
    const auto strides = c_strides(shape);

    std::vector<std::int64_t> parent;
    parent.reserve(64);
    auto find = [&](std::int64_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    for (std::ptrdiff_t i = 0; i < size; ++i) out[i] = -1;
    for_each_point(shape, fp, [&](std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p, bool interior) {
        if (!in[flat]) return;
        std::int64_t mine = -1;
        for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
            std::ptrdiff_t off;
            if (interior) {
                off = flat + fp.flat[k];
            } else {
                off = 0;
                bool outside = false;
                for (int a = 0; a < nd && !outside; ++a) {
                    const std::ptrdiff_t cc = p[a] + fp.rel[static_cast<size_t>(k * nd + a)];
                    if (cc < 0 || cc >= shape[a]) outside = true;
                    else off += cc * strides[a];
                }
                if (outside) continue;
            }
            const std::int64_t other = out[off];
            if (other < 0) continue;
            const std::int64_t root = find(other);
            if (mine < 0) {
                mine = root;
            } else if (root != mine) {
                // keep the older root: union by smallest provisional label
                if (root < mine) {
                    parent[mine] = root;
                    mine = root;
                } else {
                    parent[root] = mine;
                }
            }
        }
        if (mine < 0) {
            mine = static_cast<std::int64_t>(parent.size());
            parent.push_back(mine);
        }
        out[flat] = mine;
    });
    // compact: provisional labels are created in scan order, so numbering the
    // roots in increasing order numbers components by their first pixel --
    // which is what scipy's downward-pointing merge table produces
    std::vector<std::int64_t> final_label(parent.size(), 0);
    std::int64_t n = 0;
    for (size_t l = 0; l < parent.size(); ++l)
        if (find(static_cast<std::int64_t>(l)) == static_cast<std::int64_t>(l)) final_label[l] = ++n;
    for (std::ptrdiff_t i = 0; i < size; ++i)
        out[i] = out[i] < 0 ? 0 : final_label[static_cast<size_t>(find(out[i]))];
    return n;
}

std::vector<std::int64_t> find_objects(const std::int64_t* labels, const Shape& shape,
                                       std::int64_t max_label) {
    check_shape(shape);
    const int nd = static_cast<int>(shape.size());
    std::vector<std::int64_t> boxes(static_cast<size_t>(std::max<std::int64_t>(max_label, 0) * 2 * nd), -1);
    const std::ptrdiff_t size = shape_size(shape);
    std::vector<std::ptrdiff_t> p(nd, 0);
    for (std::ptrdiff_t flat = 0; flat < size; ++flat) {
        const std::int64_t l = labels[flat];
        if (l >= 1 && l <= max_label) {
            std::int64_t* b = &boxes[static_cast<size_t>((l - 1) * 2 * nd)];
            for (int a = 0; a < nd; ++a) {
                if (b[2 * a] < 0) {
                    b[2 * a] = p[a];
                    b[2 * a + 1] = p[a] + 1;
                } else {
                    if (p[a] < b[2 * a]) b[2 * a] = p[a];
                    if (p[a] + 1 > b[2 * a + 1]) b[2 * a + 1] = p[a] + 1;
                }
            }
        }
        for (int a = nd - 1; a >= 0; --a) {
            if (++p[a] < shape[a]) break;
            p[a] = 0;
        }
    }
    return boxes;
}

// ---------------------------------------------------------------------------
// binary morphology
// ---------------------------------------------------------------------------

bool binary_erosion(const std::uint8_t* in, const Shape& shape, const std::uint8_t* structure,
                    const Shape& sshape, const Origins& origins, int border_value, bool invert,
                    const std::uint8_t* mask, int iterations, std::uint8_t* out) {
    check_shape(shape);
    const std::ptrdiff_t ssize = shape_size(sshape);
    if (ssize < 1) throw std::invalid_argument("binary_erosion: structure must not be empty");
    std::vector<bool> keep(static_cast<size_t>(ssize));
    for (std::ptrdiff_t k = 0; k < ssize; ++k) keep[k] = structure[k] != 0;
    const Footprint fp = make_footprint(shape, sshape, origins, keep);
    const int nd = static_cast<int>(shape.size());
    const auto strides = c_strides(shape);
    const std::ptrdiff_t size = shape_size(shape);
    const bool border = border_value != 0;
    bool center = false;
    for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
        bool zero = true;
        for (int a = 0; a < nd; ++a) zero &= fp.rel[static_cast<size_t>(k * nd + a)] == 0;
        center |= zero;
    }

    // erosion: AND over the structure; dilation (invert): OR. Outside reads `border`.
    auto evaluate = [&](const std::uint8_t* src, std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p,
                        bool interior) -> std::uint8_t {
        if (mask && !mask[flat]) return src[flat] ? 1 : 0;
        for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
            bool v;
            if (interior) {
                v = src[flat + fp.flat[k]] != 0;
            } else {
                std::ptrdiff_t off = 0;
                bool outside = false;
                for (int a = 0; a < nd && !outside; ++a) {
                    const std::ptrdiff_t cc = p[a] + fp.rel[static_cast<size_t>(k * nd + a)];
                    if (cc < 0 || cc >= shape[a]) outside = true;
                    else off += cc * strides[a];
                }
                v = outside ? border : src[off] != 0;
            }
            if (!invert && !v) return 0;
            if (invert && v) return 1;
        }
        return invert ? 0 : 1;
    };

    auto full_pass = [&](const std::uint8_t* src, std::uint8_t* dst) {
        bool changed = false;
        for_each_point(shape, fp, [&](std::ptrdiff_t flat, const std::vector<std::ptrdiff_t>& p, bool interior) {
            dst[flat] = evaluate(src, flat, p, interior);
            changed |= (dst[flat] != 0) != (src[flat] != 0);
        });
        return changed;
    };

    bool changed = full_pass(in, out);
    if (iterations == 1) return changed;

    const std::ptrdiff_t limit = iterations < 1 ? size + 1 : iterations;
    std::vector<std::uint8_t> prev(out, out + size);
    if (!center) {
        // no monotonicity: every pass reads the whole previous result
        for (std::ptrdiff_t it = 1; it < limit; ++it) {
            if (iterations < 1 && !changed) break;
            std::copy(out, out + size, prev.begin());
            changed = full_pass(prev.data(), out);
        }
        return changed;
    }

    // With the centre in the structure each pass only removes (erosion) or
    // only adds (dilation), and a pixel can change in pass t+1 only if one of
    // the pixels it reads changed in pass t -- so only those are revisited.
    // Same result as repeating full passes, which is what scipy's
    // binary_erosion2 coordinate lists implement too.
    std::vector<std::ptrdiff_t> changed_list;
    for (std::ptrdiff_t i = 0; i < size; ++i)
        if ((out[i] != 0) != (in[i] != 0)) changed_list.push_back(i);
    std::vector<std::uint8_t> queued(static_cast<size_t>(size), 0);
    std::vector<std::ptrdiff_t> p(nd), candidates;
    for (std::ptrdiff_t it = 1; it < limit && !changed_list.empty(); ++it) {
        std::copy(out, out + size, prev.begin());
        candidates.clear();
        for (const std::ptrdiff_t c : changed_list) {
            std::ptrdiff_t rem = c;
            for (int a = 0; a < nd; ++a) {
                p[a] = rem / strides[a];
                rem %= strides[a];
            }
            for (std::ptrdiff_t k = 0; k < fp.n; ++k) {
                // pixels q that read c through element k: q + rel_k == c
                std::ptrdiff_t off = 0;
                bool outside = false;
                for (int a = 0; a < nd && !outside; ++a) {
                    const std::ptrdiff_t cc = p[a] - fp.rel[static_cast<size_t>(k * nd + a)];
                    if (cc < 0 || cc >= shape[a]) outside = true;
                    else off += cc * strides[a];
                }
                if (!outside && !queued[off]) {
                    queued[off] = 1;
                    candidates.push_back(off);
                }
            }
        }
        changed_list.clear();
        for (const std::ptrdiff_t q : candidates) {
            queued[q] = 0;
            std::ptrdiff_t rem = q;
            bool interior = true;
            for (int a = 0; a < nd; ++a) {
                p[a] = rem / strides[a];
                rem %= strides[a];
                if (p[a] < fp.lo[a] || p[a] >= fp.hi[a]) interior = false;
            }
            const std::uint8_t v = evaluate(prev.data(), q, p, interior);
            if (v != (prev[q] ? 1 : 0)) {
                out[q] = v;
                changed_list.push_back(q);
            }
        }
        changed = !changed_list.empty();
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Euclidean feature transform
// ---------------------------------------------------------------------------

namespace {

// _VoronoiFT on one line: `f` holds the line's feature vectors (len x rank),
// `coor` the coordinates of the line on the other axes.
void voronoi_ft(std::vector<std::ptrdiff_t>& f, std::ptrdiff_t len, const std::vector<std::ptrdiff_t>& coor,
                int rank, int d, std::vector<std::ptrdiff_t>& g, const double* sampling,
                std::vector<std::ptrdiff_t>& result) {
    auto F = [&](std::ptrdiff_t i, int j) -> std::ptrdiff_t& { return f[static_cast<size_t>(i * rank + j)]; };
    std::ptrdiff_t l = -1;
    for (std::ptrdiff_t ii = 0; ii < len; ++ii) {
        if (F(ii, 0) >= 0) {
            const double fd = static_cast<double>(F(ii, d));
            double wR = 0.0;
            for (int jj = 0; jj < rank; ++jj) {
                if (jj != d) {
                    double tw = static_cast<double>(F(ii, jj) - coor[jj]);
                    if (sampling) tw *= sampling[jj];
                    wR += tw * tw;
                }
            }
            while (l >= 1) {
                double a, b, c, uR = 0.0, vR = 0.0, f1;
                const std::ptrdiff_t idx1 = g[l];
                f1 = static_cast<double>(F(idx1, d));
                const std::ptrdiff_t idx2 = g[l - 1];
                a = f1 - static_cast<double>(F(idx2, d));
                b = fd - f1;
                if (sampling) {
                    a *= sampling[d];
                    b *= sampling[d];
                }
                c = a + b;
                for (int jj = 0; jj < rank; ++jj) {
                    if (jj != d) {
                        const double cc = static_cast<double>(coor[jj]);
                        double tu = static_cast<double>(F(idx2, jj)) - cc;
                        double tv = static_cast<double>(F(idx1, jj)) - cc;
                        if (sampling) {
                            tu *= sampling[jj];
                            tv *= sampling[jj];
                        }
                        uR += tu * tu;
                        vR += tv * tv;
                    }
                }
                if (c * vR - b * uR - a * wR - a * b * c <= 0.0) break;
                --l;
            }
            ++l;
            g[l] = ii;
        }
    }
    const std::ptrdiff_t maxl = l;
    result.assign(f.begin(), f.end());
    if (maxl >= 0) {
        l = 0;
        for (std::ptrdiff_t ii = 0; ii < len; ++ii) {
            double delta1 = 0.0, t;
            for (int jj = 0; jj < rank; ++jj) {
                t = jj == d ? static_cast<double>(F(g[l], jj) - ii) : static_cast<double>(F(g[l], jj) - coor[jj]);
                if (sampling) t *= sampling[jj];
                delta1 += t * t;
            }
            while (l < maxl) {
                double delta2 = 0.0;
                for (int jj = 0; jj < rank; ++jj) {
                    t = jj == d ? static_cast<double>(F(g[l + 1], jj) - ii)
                                : static_cast<double>(F(g[l + 1], jj) - coor[jj]);
                    if (sampling) t *= sampling[jj];
                    delta2 += t * t;
                }
                if (delta1 <= delta2) break;
                delta1 = delta2;
                ++l;
            }
            const std::ptrdiff_t idx1 = g[l];
            for (int jj = 0; jj < rank; ++jj) result[static_cast<size_t>(ii * rank + jj)] = F(idx1, jj);
        }
    }
}

}  // namespace

void euclidean_feature_transform(const std::uint8_t* in, const Shape& shape,
                                 const double* sampling, std::int32_t* features) {
    check_shape(shape);
    const int rank = static_cast<int>(shape.size());
    const std::ptrdiff_t size = shape_size(shape);
    const auto strides = c_strides(shape);
    // scipy starts from np.zeros: a foreground element carries -1 in component 0 and 0 elsewhere
    std::vector<std::ptrdiff_t> p(rank, 0);
    for (std::ptrdiff_t flat = 0; flat < size; ++flat) {
        for (int c = 0; c < rank; ++c) {
            if (in[flat]) features[c * size + flat] = c == 0 ? -1 : 0;
            else features[c * size + flat] = static_cast<std::int32_t>(p[c]);
        }
        for (int a = rank - 1; a >= 0; --a) {
            if (++p[a] < shape[a]) break;
            p[a] = 0;
        }
    }
    std::ptrdiff_t mx = 0;
    for (auto n : shape) mx = std::max(mx, n);
    std::vector<std::ptrdiff_t> f, result, g(static_cast<size_t>(mx)), coor(rank, 0);
    // axis 0 first, then 1, ...: within one axis the lines are independent,
    // so this visits them in a different order than scipy's recursion does
    // and produces the same array
    for (int d = 0; d < rank; ++d) {
        const std::ptrdiff_t len = shape[d];
        if (len == 0) continue;
        f.assign(static_cast<size_t>(len * rank), 0);
        for (const std::ptrdiff_t start : line_starts(shape, d)) {
            std::ptrdiff_t rem = start;
            for (int a = 0; a < rank; ++a) {
                coor[a] = rem / strides[a];
                rem %= strides[a];
            }
            for (std::ptrdiff_t i = 0; i < len; ++i)
                for (int c = 0; c < rank; ++c)
                    f[static_cast<size_t>(i * rank + c)] = features[c * size + start + i * strides[d]];
            voronoi_ft(f, len, coor, rank, d, g, sampling, result);
            for (std::ptrdiff_t i = 0; i < len; ++i)
                for (int c = 0; c < rank; ++c)
                    features[c * size + start + i * strides[d]] =
                        static_cast<std::int32_t>(result[static_cast<size_t>(i * rank + c)]);
        }
    }
}

// ---------------------------------------------------------------------------
// B-splines
// ---------------------------------------------------------------------------

namespace {

int filter_poles(int order, double* poles) {
    switch (order) {
        case 2: poles[0] = -0.171572875253809902396622551580603843; break;
        case 3: poles[0] = -0.267949192431122706472553658494127633; break;
        case 4:
            poles[0] = -0.361341225900220177092212841325675255;
            poles[1] = -0.013725429297339121360331226939128204;
            break;
        case 5:
            poles[0] = -0.430575347099973791851434783493520110;
            poles[1] = -0.043096288203264653822712376822550182;
            break;
        default: throw std::invalid_argument("spline order not supported");
    }
    return order / 2;
}

void init_causal_mirror(double* c, std::ptrdiff_t n, double z) {
    double z_i = z;
    const double z_n_1 = std::pow(z, static_cast<double>(n - 1));
    c[0] = c[0] + z_n_1 * c[n - 1];
    for (std::ptrdiff_t i = 1; i < n - 1; ++i) {
        c[0] += z_i * (c[i] + z_n_1 * c[n - 1 - i]);
        z_i *= z;
    }
    c[0] /= 1 - z_n_1 * z_n_1;
}

void init_anticausal_mirror(double* c, std::ptrdiff_t n, double z) {
    c[n - 1] = (z * c[n - 2] + c[n - 1]) * z / (z * z - 1);
}

void init_causal_wrap(double* c, std::ptrdiff_t n, double z) {
    double z_i = z;
    for (std::ptrdiff_t i = 1; i < n; ++i) {
        c[0] += z_i * c[n - i];
        z_i *= z;
    }
    c[0] /= 1 - z_i;
}

void init_anticausal_wrap(double* c, std::ptrdiff_t n, double z) {
    double z_i = z;
    for (std::ptrdiff_t i = 0; i < n - 1; ++i) {
        c[n - 1] += z_i * c[i];
        z_i *= z;
    }
    c[n - 1] *= z / (z_i - 1);
}

void init_causal_reflect(double* c, std::ptrdiff_t n, double z) {
    double z_i = z;
    const double z_n = std::pow(z, static_cast<double>(n));
    double sum = c[0] + z_n * c[n - 1];
    for (std::ptrdiff_t i = 1; i < n; ++i) {
        sum += z_i * (c[i] + z_n * c[n - 1 - i]);
        z_i *= z;
    }
    c[0] += sum * z / (1 - z_n * z_n);
}

void init_anticausal_reflect(double* c, std::ptrdiff_t n, double z) { c[n - 1] *= z / (z - 1); }

void apply_spline_filter(double* c, std::ptrdiff_t n, const double* poles, int npoles, int mode) {
    void (*causal)(double*, std::ptrdiff_t, double) = nullptr;
    void (*anticausal)(double*, std::ptrdiff_t, double) = nullptr;
    switch (mode) {
        case kGridConstant:
        case kConstant:
        case kMirror:
        case kWrap:
            causal = init_causal_mirror;
            anticausal = init_anticausal_mirror;
            break;
        case kGridWrap:
            causal = init_causal_wrap;
            anticausal = init_anticausal_wrap;
            break;
        case kNearest:
        case kReflect:
            causal = init_causal_reflect;
            anticausal = init_anticausal_reflect;
            break;
        default: throw std::invalid_argument("ndimage: boundary mode not supported");
    }
    double gain = 1.0;
    for (int k = 0; k < npoles; ++k) gain *= (1.0 - poles[k]) * (1.0 - 1.0 / poles[k]);
    for (std::ptrdiff_t i = 0; i < n; ++i) c[i] *= gain;
    for (int k = 0; k < npoles; ++k) {
        const double z = poles[k];
        causal(c, n, z);
        for (std::ptrdiff_t i = 1; i < n; ++i) c[i] += z * c[i - 1];
        anticausal(c, n, z);
        for (std::ptrdiff_t i = n - 2; i >= 0; --i) c[i] = z * (c[i + 1] - c[i]);
    }
}

double map_interp_coordinate(double in, std::ptrdiff_t len, int mode) {
    if (in < 0) {
        switch (mode) {
            case kMirror:
                if (len <= 1) {
                    in = 0;
                } else {
                    const double sz2 = 2.0 * len - 2;
                    in = std::fmod(in, sz2);
                    in = in <= 1 - len ? in + sz2 : -in;
                }
                break;
            case kReflect:
                if (len <= 1) {
                    in = 0;
                } else {
                    const double sz2 = 2.0 * len;
                    if (in < -sz2) in = std::fmod(in, sz2);
                    in = in < -len ? in + sz2 : (in > -1e-15 ? 1e-15 : -in) - 1;
                }
                break;
            case kWrap:
                if (len <= 1) {
                    in = 0;
                } else {
                    const std::ptrdiff_t sz = len - 1;
                    in = std::fmod(in, static_cast<double>(sz)) + static_cast<double>(sz);
                }
                break;
            case kGridWrap:
                if (len <= 1) {
                    in = 0;
                } else {
                    in = std::fmod(in, static_cast<double>(len));
                    if (in < 0) in += static_cast<double>(len);
                }
                break;
            case kNearest: in = 0; break;
            case kConstant: in = -1; break;
            default: break;
        }
    } else if (in > static_cast<double>(len - 1)) {
        switch (mode) {
            case kMirror:
                if (len <= 1) {
                    in = 0;
                } else {
                    const double sz2 = 2.0 * len - 2;
                    in = std::fmod(in, sz2);
                    if (in >= len) in = sz2 - in;
                }
                break;
            case kReflect:
                if (len <= 1) {
                    in = 0;
                } else {
                    const double sz2 = 2.0 * len;
                    in = std::fmod(in, sz2);
                    if (in >= len) in = sz2 - in - 1;
                }
                break;
            case kWrap:
                if (len <= 1) {
                    in = 0;
                } else {
                    const std::ptrdiff_t sz = len - 1;
                    in = std::fmod(in, static_cast<double>(sz));
                }
                break;
            case kGridWrap:
                if (len <= 1) in = 0;
                else in = std::fmod(in, static_cast<double>(len));
                break;
            case kNearest: in = static_cast<double>(len - 1); break;
            case kConstant: in = -1; break;
            default: break;
        }
    }
    return in;
}

void spline_weights(double x, int order, double* weights) {
    x -= std::floor(order & 1 ? x : x + 0.5);
    double y = x, z = 1.0 - x, t;
    switch (order) {
        case 1: weights[0] = 1.0 - x; break;
        case 2:
            weights[1] = 0.75 - x * x;
            y = 0.5 - x;
            weights[0] = 0.5 * y * y;
            break;
        case 3:
            weights[1] = (y * y * (y - 2.0) * 3.0 + 4.0) / 6.0;
            weights[2] = (z * z * (z - 2.0) * 3.0 + 4.0) / 6.0;
            weights[0] = z * z * z / 6.0;
            break;
        case 4:
            t = x * x;
            weights[2] = t * (t * 0.25 - 0.625) + 115.0 / 192.0;
            y = 1.0 + x;
            weights[1] = y * (y * (y * (5.0 - y) / 6.0 - 1.25) + 5.0 / 24.0) + 55.0 / 96.0;
            weights[3] = z * (z * (z * (5.0 - z) / 6.0 - 1.25) + 5.0 / 24.0) + 55.0 / 96.0;
            y = 0.5 - x;
            t = y * y;
            weights[0] = t * t / 24.0;
            break;
        case 5:
            t = y * y;
            weights[2] = t * (t * (0.25 - y / 12.0) - 0.5) + 0.55;
            t = z * z;
            weights[3] = t * (t * (0.25 - z / 12.0) - 0.5) + 0.55;
            y += 1.0;
            weights[1] = y * (y * (y * (y * (y / 24.0 - 0.375) + 1.25) - 1.75) + 0.625) + 0.425;
            z += 1.0;
            weights[4] = z * (z * (z * (z * (z / 24.0 - 0.375) + 1.25) - 1.75) + 0.625) + 0.425;
            y = 1.0 - x;
            t = y * y;
            weights[0] = y * t * t / 120.0;
            break;
        default: throw std::invalid_argument("spline order not supported");
    }
    weights[order] = 1.0;
    for (int i = 0; i < order; ++i) weights[order] -= weights[i];
}

/// The per-point body of NI_GeometricTransform / NI_ZoomShift.
class SplineSampler {
public:
    SplineSampler(const double* in, const Shape& shape, int order, int mode, double cval, int nprepad)
        : in_(in), shape_(shape), order_(order), mode_(mode), cval_(cval), nprepad_(nprepad) {
        check_shape(shape);
        if (order < 0 || order > 5) throw std::invalid_argument("spline order not supported");
        check_mode(mode);
        rank_ = static_cast<int>(shape.size());
        strides_ = c_strides(shape);
        spline_mode_ = (mode == kConstant || mode == kWrap) ? kMirror : mode;
        filter_size_ = 1;
        for (int j = 0; j < rank_; ++j) filter_size_ *= order + 1;
        fcoord_.resize(static_cast<size_t>(filter_size_ * rank_));
        foffset_.resize(static_cast<size_t>(filter_size_));
        std::vector<std::ptrdiff_t> ftmp(rank_, 0);
        std::ptrdiff_t kk = 0;
        for (std::ptrdiff_t hh = 0; hh < filter_size_; ++hh) {
            for (int j = 0; j < rank_; ++j) fcoord_[static_cast<size_t>(j + hh * rank_)] = ftmp[j];
            foffset_[hh] = kk;
            for (int j = rank_ - 1; j >= 0; --j) {
                if (ftmp[j] < order) {
                    ftmp[j]++;
                    kk += strides_[j];
                    break;
                } else {
                    ftmp[j] = 0;
                    kk -= strides_[j] * order;
                }
            }
        }
        splvals_.assign(static_cast<size_t>(rank_ * (order + 1)), 0.0);
        edge_offsets_.assign(static_cast<size_t>(rank_ * (order + 1)), 0);
        has_edge_.assign(rank_, false);
        edge_const_.assign(static_cast<size_t>(rank_ * (order + 1)), 0);
    }

    double sample(const double* icoor) {
        for (int h = 0; h < rank_; ++h)
            if (!std::isfinite(icoor[h])) throw std::invalid_argument("coordinates must be finite");
        const int order = order_;
        bool edge = false;
        std::ptrdiff_t offset = 0;
        std::fill(edge_const_.begin(), edge_const_.end(), 0);
        for (int h = 0; h < rank_; ++h) {
            double cc = icoor[h] + nprepad_;
            if (mode_ != kGridConstant && mode_ != kNearest) {
                cc = map_interp_coordinate(cc, shape_[h], mode_);
            } else {
                cc = std::fmax(cc, -order - 1.0);
                cc = std::fmin(cc, static_cast<double>(shape_[h]) + order + 1.0);
            }
            if (cc > -1.0 || mode_ == kGridConstant || mode_ == kNearest) {
                std::ptrdiff_t start;
                if (order & 1) start = static_cast<std::ptrdiff_t>(std::floor(cc)) - order / 2;
                else start = static_cast<std::ptrdiff_t>(std::floor(cc + 0.5)) - order / 2;
                offset += strides_[h] * start;
                has_edge_[h] = false;
                if (mode_ == kGridConstant) {
                    for (int l = 0; l <= order; ++l) {
                        const std::ptrdiff_t idx = start + l;
                        edge_const_[static_cast<size_t>(h * (order + 1) + l)] = (idx < 0 || idx >= shape_[h]);
                    }
                } else if (start < 0 || start + order >= shape_[h]) {
                    edge = true;
                    has_edge_[h] = true;
                    for (int l = 0; l <= order; ++l) {
                        const std::ptrdiff_t idx = static_cast<std::ptrdiff_t>(
                            map_interp_coordinate(static_cast<double>(start + l), shape_[h], spline_mode_));
                        edge_offsets_[static_cast<size_t>(h * (order + 1) + l)] = strides_[h] * (idx - start);
                    }
                }
                if (order != 0) spline_weights(cc, order, &splvals_[static_cast<size_t>(h * (order + 1))]);
            } else {
                return cval_;
            }
        }
        double t = 0.0;
        const std::ptrdiff_t* ff = fcoord_.data();
        for (std::ptrdiff_t hh = 0; hh < filter_size_; ++hh) {
            double coeff = 0.0;
            bool is_cval = false;
            if (mode_ == kGridConstant) {
                for (int l = 0; l < rank_; ++l)
                    if (edge_const_[static_cast<size_t>(l * (order + 1) + ff[l])]) is_cval = true;
            }
            if (is_cval) {
                coeff = cval_;
            } else {
                std::ptrdiff_t idx = 0;
                if (edge) {
                    for (int l = 0; l < rank_; ++l) {
                        if (has_edge_[l]) idx += edge_offsets_[static_cast<size_t>(l * (order + 1) + ff[l])];
                        else idx += ff[l] * strides_[l];
                    }
                } else {
                    idx = foffset_[hh];
                }
                idx += offset;
                coeff = in_[idx];
            }
            for (int l = 0; l < rank_; ++l)
                if (order > 0) coeff *= splvals_[static_cast<size_t>(l * (order + 1) + ff[l])];
            t += coeff;
            ff += rank_;
        }
        return t;
    }

    int rank() const { return rank_; }

private:
    const double* in_;
    Shape shape_;
    int order_, mode_, spline_mode_ = kMirror, rank_ = 0;
    double cval_;
    int nprepad_;
    std::vector<std::ptrdiff_t> strides_, fcoord_, foffset_, edge_offsets_;
    std::ptrdiff_t filter_size_ = 1;
    std::vector<double> splvals_;
    std::vector<bool> has_edge_;
    std::vector<char> edge_const_;
};

}  // namespace

void spline_filter1d(double* data, const Shape& shape, int axis, int order, int mode) {
    check_shape(shape);
    check_axis(shape, axis);
    check_mode(mode);
    if (order < 2) return;  // orders 0 and 1 are their own coefficients
    double poles[2];
    const int npoles = filter_poles(order, poles);
    const std::ptrdiff_t len = shape[axis];
    if (len <= 1) return;
    const std::ptrdiff_t stride = c_strides(shape)[axis];
    std::vector<double> line(static_cast<size_t>(len));
    for (const std::ptrdiff_t start : line_starts(shape, axis)) {
        for (std::ptrdiff_t i = 0; i < len; ++i) line[i] = data[start + i * stride];
        apply_spline_filter(line.data(), len, poles, npoles, mode);
        for (std::ptrdiff_t i = 0; i < len; ++i) data[start + i * stride] = line[i];
    }
}

void map_coordinates(const double* in, const Shape& shape, const double* coordinates,
                     std::ptrdiff_t n_points, int order, int mode, double cval, int nprepad,
                     double* out) {
    SplineSampler sampler(in, shape, order, mode, cval, nprepad);
    const int rank = sampler.rank();
    std::vector<double> icoor(rank);
    for (std::ptrdiff_t m = 0; m < n_points; ++m) {
        for (int h = 0; h < rank; ++h) icoor[h] = coordinates[h * n_points + m];
        out[m] = sampler.sample(icoor.data());
    }
}

void shift(const double* in, const Shape& shape, const double* shifts, int order, int mode,
           double cval, int nprepad, double* out) {
    SplineSampler sampler(in, shape, order, mode, cval, nprepad);
    const int rank = sampler.rank();
    Shape oshape(rank);
    for (int h = 0; h < rank; ++h) {
        oshape[h] = shape[h] - 2 * static_cast<std::ptrdiff_t>(nprepad);
        if (oshape[h] < 0) throw std::invalid_argument("shift: padding larger than the array");
    }
    const std::ptrdiff_t size = shape_size(oshape);
    std::vector<std::ptrdiff_t> p(rank, 0);
    std::vector<double> icoor(rank);
    for (std::ptrdiff_t flat = 0; flat < size; ++flat) {
        // NI_ZoomShift: (double)kk + shift, where scipy passes shift = -s
        for (int h = 0; h < rank; ++h) icoor[h] = static_cast<double>(p[h]) + (-shifts[h]);
        out[flat] = sampler.sample(icoor.data());
        for (int a = rank - 1; a >= 0; --a) {
            if (++p[a] < oshape[a]) break;
            p[a] = 0;
        }
    }
}

}  // namespace ndimage
}  // namespace tttrlib
