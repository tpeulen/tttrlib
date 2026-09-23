// SPDX-License-Identifier: BSD-3-Clause
//
// The image kernels are header-only (ImageOps.h and the headers it wraps), so
// their registry entry needs a translation unit of its own; this is it.

#include "ImageOps.h"
#include "Registry.h"

// ---- registry entries (Registry.h, core): declared next to the code, registered
// when this library loads; a static consumer links the archive whole.
namespace {
const char* const kImageKernelsEntry = R"JSON({
  "name": "image_kernels",
  "label": "Image kernels: rank filters, resize, blur, gradients, warp, drift",
  "summary": "2-D rank filters, summed-area tables, area/bilinear/bicubic resize, 3-box gaussian, Sobel/Laplace gradients, affine warp, value histograms and moments, and SAD drift estimation.",
  "description": "Header-only image kernels ported from the concepts of ermig1979/Simd (MIT; re-expressed std-only): median/min/max/midpoint filters over 3x3 rhomb, 3x3 and 5x5 windows (integer medians on a sliding histogram), summed-area tables with O(1) rectangle sums, separable area, bilinear and bicubic (Keys a=-0.5) resampling, a running-sum 3-box gaussian, Sobel x/y and Laplace-8 gradients, an inverse-mapped bilinear affine warp (OpenCV 2x3 convention), value histograms, intensity-weighted moments, and a pyramid SAD translation search with parabolic sub-pixel refinement for frame drift. Each kernel is A/B-tested against a brute-force reference in test/cpp/; the NumPy-level parity is pinned in test/python/misc/test_image_ops.py.",
  "operation_type": "image_analysis",
  "method": "median_filter",
  "params_schema": {
    "type": "object",
    "properties": {
      "shape": {
        "type": "integer",
        "title": "Rank-filter window (0 rhomb 3x3, 1 square 3x3, 2 square 5x5)",
        "default": 1
      },
      "sigma": {
        "type": "number",
        "title": "Gaussian sigma (pixels)",
        "default": 1.0
      }
    }
  },
  "inputs": {
    "required": [
      "image"
    ]
  },
  "outputs": {
    "columns": [
      "image"
    ]
  },
  "row_grain": "pixel",
  "references": [
    {
      "type": "software",
      "authors": "Ihar Yermalayeu",
      "title": "Simd Library",
      "url": "https://github.com/ermig1979/Simd"
    },
    {
      "type": "journal",
      "authors": "Keys, R.",
      "title": "Cubic convolution interpolation for digital image processing",
      "journal": "IEEE Trans Acoust Speech Signal Process",
      "year": 1981,
      "volume": "29",
      "pages": "1153-1160"
    }
  ],
  "api": [
    "median_filter", "median_filter_f64", "median_filter_u16",
    "min_filter", "min_filter_f64", "min_filter_u16",
    "max_filter", "max_filter_f64", "max_filter_u16",
    "midpoint_filter", "midpoint_filter_f64", "midpoint_filter_u16",
    "integral_image_u16", "rect_sum_u64",
    "resize", "resize_area_f64", "resize_area_u16",
    "resize_bilinear_f64", "resize_bilinear_u16",
    "resize_bicubic_f64", "resize_bicubic_u16",
    "gaussian_blur", "gaussian_blur_f64",
    "sobel_dx", "sobel_dx_f64", "sobel_dy", "sobel_dy_f64",
    "laplace", "laplace8_f64",
    "warp_affine", "warp_affine_f64",
    "image_histogram", "value_histogram_f64", "value_histogram_u16",
    "moments", "image_moments_f64",
    "estimate_drift", "estimate_drift_f64"
  ],
  "can_replay": false
})JSON";
bool register_image_kernel_entries() {
    tttrlib::register_algorithm_json("math", "image_kernels", kImageKernelsEntry);
    return true;
}
const bool kImageKernelsRegistered = register_image_kernel_entries();
}  // namespace
