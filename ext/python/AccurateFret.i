// SPDX-License-Identifier: BSD-3-Clause
%{
#include "AccurateFret.h"
#include "AccurateFretPopulations.h"
%}

// the raw kernels take flat vectors and return structs; the Python layer in
// AccurateFret.py gives them numpy-in / dict-out signatures under the public names
%rename(_afret_apparent_es) tttrlib::apparent_es;
%rename(_afret_corrected_es) tttrlib::corrected_es;
%rename(_afret_corrected_es_matrix) tttrlib::corrected_es_matrix;
%rename(_afret_corrected_es_general) tttrlib::corrected_es_general;
%rename(_afret_gaussian_mixture_1d) tttrlib::gaussian_mixture_1d;
%rename(_afret_best_gaussian_mixture_1d) tttrlib::best_gaussian_mixture_1d;
%rename(_afret_classify_es_populations) tttrlib::classify_es_populations;
%rename(_afret_split_fret_subpopulations) tttrlib::split_fret_subpopulations;
%rename(_afret_leakage_from_donor_only) tttrlib::leakage_from_donor_only;
%rename(_afret_direct_excitation_from_acceptor_only) tttrlib::direct_excitation_from_acceptor_only;
%rename(FretPopulationSplit) tttrlib::PopulationSplit;

namespace tttrlib {};
%include "AccurateFret.h"
%include "AccurateFretPopulations.h"

#ifdef SWIGPYTHON
%pythoncode "./ext/python/AccurateFret.py"
#endif
