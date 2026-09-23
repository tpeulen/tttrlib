// SPDX-License-Identifier: BSD-3-Clause
%{
#include "AccurateFret.h"
#include "AccurateFretPopulations.h"
#include "AccurateFretCalibrate.h"
#include "AccurateFretUncertainty.h"
#include "AccurateFretMultiDim.h"
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
%rename(_afret_global_es_correction) tttrlib::global_es_correction;
%rename(_afret_beta_from_stoichiometry) tttrlib::beta_from_stoichiometry;
%rename(_afret_gamma_from_lifetime) tttrlib::gamma_from_lifetime;
%rename(_afret_lightpath_correction_factors) tttrlib::lightpath_correction_factors;
%rename(_afret_auto_calibrate) tttrlib::auto_calibrate;
%rename(_afret_auto_calibrate_start) tttrlib::auto_calibrate_start;
%rename(_afret_auto_calibrate_iterate) tttrlib::auto_calibrate_iterate;
%rename(_afret_auto_calibrate_cancel) tttrlib::auto_calibrate_cancel;
%rename(_afret_auto_calibrate_finish) tttrlib::auto_calibrate_finish;
%rename(_afret_auto_calibrate_bootstrap) tttrlib::auto_calibrate_bootstrap;
%rename(_afret_efficiency_uncertainty) tttrlib::efficiency_uncertainty;
%rename(_afret_distance_from_efficiency) tttrlib::distance_from_efficiency;
%rename(_afret_accurate_fret) tttrlib::accurate_fret;
%rename(_afret_refine_gamma) tttrlib::refine_gamma;
%rename(_afret_gaussian_mixture_nd) tttrlib::gaussian_mixture_nd;
%rename(_afret_best_gaussian_mixture_nd) tttrlib::best_gaussian_mixture_nd;
%rename(_afret_classify_populations_nd) tttrlib::classify_populations_nd;
%rename(_afret_auto_calibrate_set_dimensions) tttrlib::auto_calibrate_set_dimensions;

namespace tttrlib {};
%include "AccurateFret.h"
%include "AccurateFretPopulations.h"
%include "AccurateFretCalibrate.h"
%template(VectorFretPopulation) std::vector<tttrlib::FretPopulation>;
%include "AccurateFretUncertainty.h"
%include "AccurateFretMultiDim.h"

#ifdef SWIGPYTHON
%pythoncode "./ext/python/AccurateFret.py"
%pythoncode "./ext/python/AccurateFretCalibrate.py"
%pythoncode "./ext/python/AccurateFretMultiDim.py"
#endif
