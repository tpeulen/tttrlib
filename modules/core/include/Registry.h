// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_REGISTRY_H
#define TTTRLIB_REGISTRY_H

#include <string>
#include <vector>

#include "RegistryCore.h"

/*!
 * \file Registry.h
 * \brief The registry: what tttrlib can do, by name, as data.
 *
 * There is ONE registry, and it lives here in core. Every algorithm, fit
 * model, fit-setup block, objective, prior kind, correlation method and
 * pipeline operation registers itself in it -- next to its own code, when its
 * library loads (`register_algorithm` / `register_algorithm_json`) -- and so
 * does every capability a plugin brings (the plugin host records the
 * declaration, the registry pulls it). Nothing is described in a literal
 * anywhere else, so nothing can drift from what actually registered.
 *
 * `registry_json()` assembles the whole thing: one JSON object per capability
 * that registered, keyed by name, plus the three catalogs that are not
 * algorithms -- `file_container` (the I/O format table), `table_format` and
 * `plugin` (load status). Each entry carries at least `name`, `label`,
 * `summary`, `provider`; a callable one also `method` and `params_schema`.
 *
 * Registration happens when a module's library is loaded (a static
 * initialiser next to the registration), which is why a consumer that links
 * `libtttrlib_static.a` must link it whole (`--whole-archive` /
 * `-force_load`; the R package does) -- an archive member nothing references
 * is otherwise dropped, and its entries with it. In-tree static builds link
 * the module objects for the same reason.
 */

namespace tttrlib {

// AlgorithmDescriptor and algorithm_key are in RegistryCore.h, the mechanism shared with other
// libraries (imp.bff vendors it); this header is tttrlib's registry built on it.

/*!
 * \brief Register one algorithm.
 *
 * \return false if the descriptor is incomplete (no operation_type or no
 *         capability) or if its key (`name`, else `operation_type`) is already
 *         registered. A
 *         duplicate is rejected rather than overwritten: two algorithms
 *         answering to one name is not a preference the registry can resolve,
 *         and silently keeping the last one registered makes the result depend
 *         on load order.
 */
bool register_algorithm(const AlgorithmDescriptor& desc);

/// Every registered algorithm of one capability, as a JSON object keyed by
/// `operation_type`. `{}` when nothing of that capability is registered.
/*!
 * \brief Register one algorithm from a complete JSON entry.
 *
 * The entry is what `registry("<capability>")["<key>"]` will show: the
 * generic descriptor fields are read from it (`label`, `summary`,
 * `description`, `operation_type` (defaults to \p key), `params_schema` /
 * `settings_schema`, `inputs`, `outputs`, `row_grain`, `can_replay`,
 * `method`) and the whole object is carried as `extra_json`, so every key
 * the entry had is emitted back exactly. This is how the fit models, the
 * fit-setup blocks, the objectives and the pipeline operations declare
 * themselves next to their code -- one registration path for everything
 * the registry lists, and no hand-authored registry literal anywhere.
 *
 * An entry's optional `api` array names the symbols it owns, and those names
 * are **Python** paths resolved from the `tttrlib` module -- `Class.method`,
 * with a dot, and a nested C++ type by the flat name SWIG gives it
 * (`BurstML::FitResult` crosses as `BurstMLFitResult`). Writing C++ notation
 * here resolves to nothing and leaves the entry documenting an API no caller
 * can reach; `test_registry_completeness.py::test_every_api_symbol_exists`
 * fails on it and says so.
 *
 * \return as register_algorithm; also false if \p entry_json is not a JSON
 *         object.
 */
bool register_algorithm_json(const std::string& capability, const std::string& key,
                             const std::string& entry_json);


std::string algorithms_json(const std::string& capability);

/// The capabilities that have at least one registration, in registration order.
std::vector<std::string> algorithm_capabilities();

/// Look up one registration. Null when it is not registered.
/// The descriptor registered under \p key (`name`, else `operation_type`), or nullptr.
const AlgorithmDescriptor* find_algorithm(const std::string& key);

/// The `can_replay` registrations, in the shape the `operation` registry
/// category uses, so the provenance system reads live registrations and
/// hand-authored entries through one accessor.
std::string algorithm_operations_json();

// ---------------------------------------------------------------- assembly --

/*!
 * \brief The whole registry: `{category: {name: entry}}`, as a JSON string.
 *
 * Each entry carries at least `name`, `label` and `summary`; entries describing
 * something callable also carry `method` and a `params_schema`.
 */
std::string registry_json();

/*!
 * \brief The lifetime fit models (`fit` category), as a JSON string.
 *
 * Assembled from the entries the decay module registered about its models
 * (the entries in `DecayFitModelFit2x.cpp` / `DecayFitModelNExp.cpp`), plus a plugin's. Each
 * carries `params_schema` in `initial_values` order and a `setup` link to the
 * shared construction inputs.
 */
std::string fit_models_json();

/*!
 * \brief The shared construction inputs (`fit_setup` category), as JSON.
 *
 * Referenced by each `fit` entry's `setup` link; declared next to the models.
 */
std::string fit_setup_json();

/*!
 * \brief The selectable fit objectives (`objective` category), as a JSON string.
 *
 * Which statistic a fit minimises, named rather than encoded as flags. From
 * declared next to the statistics (`DecayStatistics.cpp`).
 */
std::string fit_objectives_json();

/*!
 * \brief The pipeline operation catalog (`operation` category), as JSON.
 *
 * Describes each analysis step that can appear in a burst pipeline .pto:
 * its operation_type (matching ``_mmfdb_operation.operation_type`` in PTO
 * tags), inputs, outputs (column names matching mmfdb.dic), data_format,
 * row_grain and settings schema -- the machine-readable contract between the
 * ``tttr`` CLI that writes .pto artifacts, chiSurf plugins that read/produce
 * them, ndx that consumes the output columns and the provenance reader that
 * replays a pipeline. ``data_format`` is **storage** and is ``dstore`` for
 * every built-in (never a legacy companion suffix; mmfdb declares none).
 *
 * Every entry is a registration made next to the code that performs the
 * operation (burst, decay, fcs modules) or by a plugin
 * (`tttrlib_operation_v1`); the built-in ones are the `can_replay`
 * registrations of the `operation` capability.
 */
std::string operation_registry_json();

/*!
 * \brief One category of the registry, as a JSON string.
 * \return `{}` when the category does not exist.
 */
std::string registry_category_json(const std::string& category);

/*!
 * \brief Names of the available registry categories.
 */
std::vector<std::string> registry_categories();

} // namespace tttrlib

#endif // TTTRLIB_REGISTRY_H
