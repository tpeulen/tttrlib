// SPDX-License-Identifier: BSD-3-Clause
%{
#include "Embedding.h"
%}

// t-SNE and UMAP (Embedding.h). The raw kernels take flat typed arrays and
// return ARGOUTVIEWM buffers, so the same %apply lines serve r and js
// (rarrays.i, jsarrays.i); Python adds `tsne`, `umap` and the image helpers
// below, in the keyword style of scikit-learn and umap-learn.

%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* data, int n_samples, int n_features)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* init, int init_rows, int init_cols)}
%apply (double* IN_ARRAY2, int DIM1, int DIM2) {(const double* embedding, int emb_rows, int emb_cols)}
%apply (double** ARGOUTVIEWM_ARRAY2, int* DIM1, int* DIM2) {(double** out_P, int* n_rows, int* n_cols)}
%apply (double** ARGOUTVIEWM_ARRAY2, int* DIM1, int* DIM2) {(double** out_embedding, int* n_out_rows, int* n_out_cols)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_row, int* n_out_row)}
%apply (long long** ARGOUTVIEWM_ARRAY1, int* DIM1) {(long long** out_col, int* n_out_col)}
%apply (double** ARGOUTVIEWM_ARRAY1, int* DIM1) {(double** out_value, int* n_out_value)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* row, int n_row)}
%apply (long long* IN_ARRAY1, int DIM1) {(const long long* col, int n_col)}
%apply (double* IN_ARRAY1, int DIM1) {(const double* value, int n_value)}
%apply (double** ARGOUTVIEWM_ARRAY2, int* DIM1, int* DIM2) {(double** out_layout, int* n_out_rows, int* n_out_cols)}
%apply (double* OUTPUT) {(double* kl_divergence)}
%apply (int* OUTPUT) {(int* n_iter)}
%apply (double* OUTPUT) {(double* a)}
%apply (double* OUTPUT) {(double* b)}

%include "Embedding.h"

%clear (double* a);
%clear (double* b);

#ifdef SWIGPYTHON
%pythoncode "./ext/python/Embedding.py"
#endif
