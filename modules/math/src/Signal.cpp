// SPDX-License-Identifier: BSD-3-Clause
//
// Ports of scipy's _lfilter.cc (filt) and of FITPACK as scipy carries it in
// C (dfitpack.c). See Signal.h. FP contraction ON: scipy's wheels contract
// these multiply-adds into fused ones, and the parity test is bit-exact.
#pragma STDC FP_CONTRACT ON

#include "Signal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace {

double* new_view(int n, double** out, int* n_out) {
  double* buffer = static_cast<double*>(std::calloc(n > 0 ? n : 1, sizeof(double)));
  if (buffer == nullptr) throw std::bad_alloc();
  *out = buffer;
  *n_out = n;
  return buffer;
}

// ---- FITPACK (dfitpack.c), 1-based index arithmetic kept as in the source --

void fpbspl(const double* t, int k, double x, int l, double* h) {
  int li, lj;
  double f;
  double hh[19] = {0.0};
  h[0] = 1.0;
  for (int j = 1; j <= k; j++) {
    for (int i = 1; i <= j; i++) hh[i - 1] = h[i - 1];
    h[0] = 0.0;
    for (int i = 1; i <= j; i++) {
      li = l + i;
      lj = li - j;
      if (t[li - 1] != t[lj - 1]) {
        f = hh[i - 1] / (t[li - 1] - t[lj - 1]);
        h[i - 1] = h[i - 1] + f * (t[li - 1] - x);
        h[i] = f * (x - t[lj - 1]);
      } else {
        h[i] = 0.0;
      }
    }
  }
}

void fpgivs(double piv, double* ww, double* cos, double* sin) {
  double dd;
  const double one = 1.0;
  double store = std::fabs(piv);
  if (store >= *ww) {
    dd = store * std::sqrt(one + (*ww / piv) * (*ww / piv));
  } else {
    dd = *ww * std::sqrt(one + (piv / *ww) * (piv / *ww));
  }
  *cos = *ww / dd;
  *sin = piv / dd;
  *ww = dd;
}

void fprota(double c, double s, double* a, double* b) {
  double temp = c * (*a) - s * (*b);
  *b = c * (*b) + s * (*a);
  *a = temp;
}

void fpback(const double* a, const double* z, int n, int k, double* c, int nest) {
  c[n - 1] = z[n - 1] / a[n - 1];
  if (n == 1) return;
  int k1 = k - 1;
  int i = n - 1;
  for (int j = 2; j <= n; j++) {
    double store = z[i - 1];
    int i1 = k1;
    if (j <= k1) i1 = j - 1;
    int m = i;
    for (int l = 1; l <= i1; l++) {
      m++;
      store = store - c[m - 1] * a[(i - 1) + l * nest];
    }
    c[i - 1] = store / a[i - 1];
    i--;
  }
}

void fpchec(const double* x, int m, const double* t, int n, int k, int* ier) {
  int k1 = k + 1;
  int k2 = k1 + 1;
  int nk1 = n - k1;
  int nk2 = nk1 + 1;
  *ier = 10;
  if ((nk1 < k1) || (nk1 > m)) { *ier = 10; return; }
  int j = n;
  for (int i = 1; i <= k; i++) {
    if (t[i - 1] > t[i]) { *ier = 20; return; }
    if (t[j - 1] < t[j - 2]) { *ier = 20; return; }
    j--;
  }
  for (int i = k2; i <= nk2; i++) {
    if (t[i - 1] <= t[i - 2]) { *ier = 30; return; }
  }
  if ((x[0] < t[k]) || (x[m - 1] > t[nk2 - 1])) { *ier = 40; return; }
  if ((x[0] >= t[k2 - 1]) || (x[m - 1] <= t[nk1 - 1])) { *ier = 50; return; }
  int i = 1;
  int l = k2;
  int nk3 = nk1 - 1;
  if (nk3 < 2) { *ier = 0; return; }
  for (int jj = 2; jj <= nk3; jj++) {
    double tj = t[jj - 1];
    l++;
    double tl = t[l - 1];
    do {
      i++;
      if (i >= m) { *ier = 50; return; }
    } while (x[i - 1] <= tj);
    if (x[i - 1] >= tl) { *ier = 50; return; }
  }
  *ier = 0;
}

}  // namespace

void signal_lfilter(const double* b, int n_b, const double* a, int n_a,
                    const double* src, int n_src, double** out, int* n_out) {
  if (n_b < 1 || n_a < 1) throw std::invalid_argument("lfilter: b and a must be non-empty");
  if (a[0] == 0.0) {
    throw std::invalid_argument("lfilter: filter coefficient a[0] == 0 not supported");
  }
  const int nfilt = std::max(n_a, n_b);
  // zfill, then normalise once by a0 (scipy divides b and a in place).
  std::vector<double> bz(nfilt, 0.0), az(nfilt, 0.0), zz(nfilt > 1 ? nfilt - 1 : 1, 0.0);
  std::copy(b, b + n_b, bz.begin());
  std::copy(a, a + n_a, az.begin());
  const double a0 = az[0];
  for (int n = 0; n < nfilt; ++n) {
    bz[n] /= a0;
    az[n] /= a0;
  }
  double* y = new_view(n_src, out, n_out);
  double* Z = zz.data();
  for (int kx = 0; kx < n_src; kx++) {
    const double* ptr_b = bz.data();
    const double* ptr_a = az.data();
    const double xn = src[kx];
    double* yn = &y[kx];
    if (nfilt > 1) {
      double* ptr_Z = Z;
      *yn = *ptr_Z + *ptr_b * xn;
      ptr_b++;
      ptr_a++;
      for (int n = 0; n < nfilt - 2; n++) {
        *ptr_Z = ptr_Z[1] + xn * (*ptr_b) - *yn * (*ptr_a);
        ptr_b++;
        ptr_a++;
        ptr_Z++;
      }
      *ptr_Z = xn * (*ptr_b) - *yn * (*ptr_a);
    } else {
      *yn = xn * (*ptr_b);
    }
  }
}

int signal_curfit_fixed(const double* x, int n_x, const double* y, int n_y,
                        const double* w, int n_w, double xb, double xe, int k,
                        const double* interior, int n_interior,
                        double** out_t, int* n_out_t, double** out_c, int* n_out_c,
                        double* fp_out) {
  const int m = n_x;
  const int n = n_interior + 2 * k + 2;
  double* t = new_view(n, out_t, n_out_t);
  double* c = new_view(n, out_c, n_out_c);
  *fp_out = 0.0;
  // curfit's argument checks (iopt = -1).
  if (n_y != m || n_w != m) return 10;
  if (k <= 0 || k > 5) return 10;
  const int k1 = k + 1;
  const int nmin = 2 * k1;
  const int nest = n;
  if (m < k1 || nest < nmin) return 10;
  if (xb > x[0] || xe < x[m - 1]) return 10;
  for (int i = 1; i < m; i++) {
    if (x[i - 1] > x[i]) return 10;
  }
  if (n < nmin || n > nest) return 10;
  for (int i = 0; i < n_interior; ++i) t[k1 + i] = interior[i];
  int j = n;
  for (int i = 0; i < k1; i++) {
    t[i] = xb;
    t[j - 1] = xe;
    j = j - 1;
  }
  int ier = 0;
  fpchec(x, m, t, n, k, &ier);
  if (ier != 0) return ier;

  // fpcurf, iopt < 0: one least-squares pass over the given knots.
  const int nk1 = n - k1;
  std::vector<double> z(nest, 0.0), a(static_cast<size_t>(nest) * k1, 0.0);
  double h[7];
  double fp = 0.0;
  double ccos, ssin, piv, xi, wi, yi;
  int l = k1;
  for (int it = 1; it <= m; it++) {
    xi = x[it - 1];
    wi = w[it - 1];
    yi = y[it - 1] * wi;
    while (!((xi < t[l]) || (l == nk1))) l++;
    fpbspl(t, k, xi, l, h);
    for (int i = 1; i <= k1; i++) h[i - 1] = h[i - 1] * wi;
    int jj = l - k1;
    for (int i = 1; i <= k1; i++) {
      jj++;
      piv = h[i - 1];
      if (piv == 0.0) continue;
      fpgivs(piv, &a[jj - 1], &ccos, &ssin);
      fprota(ccos, ssin, &yi, &z[jj - 1]);
      if (i == k1) break;
      int i2 = 1;
      int i3 = i + 1;
      for (int i1 = i3; i1 <= k1; i1++) {
        i2++;
        fprota(ccos, ssin, &h[i1 - 1], &a[(jj - 1) + (i2 - 1) * nest]);
      }
    }
    fp = fp + yi * yi;
  }
  fpback(a.data(), z.data(), nk1, k1, c, nest);
  *fp_out = fp;
  return 0;
}

int signal_splev(const double* t, int n_t, const double* c, int n_c, int k,
                 const double* x, int n_x, int ext, double** out, int* n_out) {
  (void)n_c;
  double* y = new_view(n_x, out, n_out);
  const int n = n_t;
  const int m = n_x;
  if (m < 1) return 10;
  const int k1 = k + 1;
  const int k2 = k1 + 1;
  const int nk1 = n - k1;
  const double tb = t[k];
  const double te = t[nk1];
  int l = k1;
  int l1 = l + 1;
  double h[20];
  for (int i = 1; i <= m; i++) {
    double arg = x[i - 1];
    if ((arg < tb) || (arg > te)) {
      if (ext == 1) {
        y[i - 1] = 0.0;
        continue;
      } else if (ext == 2) {
        return 1;
      } else if (ext == 3) {
        arg = (arg < tb) ? tb : te;
      }
    }
    while (!((arg >= t[l - 1]) || (l1 == k2))) {
      l1 = l;
      l--;
    }
    while (!((arg < t[l1 - 1]) || (l == nk1))) {
      l = l1;
      l1 = l + 1;
    }
    fpbspl(t, k, arg, l, h);
    double sp = 0.0;
    int ll = l - k1;
    for (int jj = 1; jj <= k1; jj++) {
      ll++;
      sp = sp + c[ll - 1] * h[jj - 1];
    }
    y[i - 1] = sp;
  }
  return 0;
}
