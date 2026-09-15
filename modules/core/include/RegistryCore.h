// SPDX-License-Identifier: BSD-3-Clause
#ifndef TTTRLIB_REGISTRYCORE_H
#define TTTRLIB_REGISTRYCORE_H

/*!
 * \file RegistryCore.h
 * \brief The registry's mechanism: descriptors, the table they register in, and the entry shape.
 *
 * What a library can do, by name, as data -- the part of `Registry.h` that knows nothing about
 * what is registered. tttrlib's registry (`Registry.h` / `Registry.cpp`: plugins, file formats,
 * the category order its bindings pin) is one user; any library that wants its algorithms
 * discoverable the same way is another (imp.bff vendors this header, pinned, and keeps its own
 * table). One mechanism, so a consumer reads every library's registry with the same code.
 *
 * Header-only and std-only: the table is a template on the JSON type, so this header includes
 * no JSON library. Both users instantiate it with `nlohmann::ordered_json` -- key order is part
 * of the surface (a fit's parameter flattening depends on it).
 *
 * The emitted entry, per registration (`AlgorithmTable::entry_of`):
 * `name`, `label`, `method` (only when there is one), `summary`, `description`, `params_schema`
 * and `settings_schema` (JSON Schema), `capability`, `operation_type`, `row_grain`, `inputs`,
 * `outputs`, `references`, `provider`, `can_replay`, then the capability-specific keys of
 * `extra_json`, which win.
 *
 * Moved out of `Registry.h` / `Registry.cpp` on 2026-09-15 (imp.bff PRD-147 amendment A2) with no
 * change to what is emitted; the one behavioural change is that `find` now returns a pointer that
 * stays valid (the store was a vector, whose growth moved the descriptors a returned pointer
 * pointed at).
 */

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tttrlib {

/*!
 * \brief What an algorithm declares about itself.
 *
 * Every field except `impl` is data the registry serves to consumers: a UI
 * renders `display_name`, `summary`, `description` and `references_json`; a
 * form builder renders `settings_schema`; the provenance system reads
 * `operation_type`, `row_grain`, `inputs_json`, `outputs_json` and
 * `can_replay`.
 *
 * `impl` is opaque to the host. A capability registrar knows
 * how to turn it into the C++ object or call for its capability — which is the
 * seam that lets a C ABI plugin table and a C++ built-in factory travel one
 * registration path.
 */
struct AlgorithmDescriptor {
    // -- identity (mmfdb / flrCIF canonical names) --
    /// The mmfdb `operation_type` this algorithm performs, e.g.
    /// "fcs_correlation". Not necessarily unique: two entries can perform the
    /// same operation type on different inputs (the burst-MLE fit on the green
    /// and on the red detector both are `burst_lifetime_fitting`).
    std::string operation_type;
    /// The registry key -- what a caller names the entry by (`registry("fit")
    /// ["fit23"]`, `burst_search_by_name("maxtree")`). Unique within the
    /// registry. Empty means "same as operation_type", which is the common
    /// case; emitted as `name`.
    std::string name;
    std::string display_name;     ///< human label
    std::string summary;          ///< one line, for lists and tooltips

    // -- human documentation --
    /// Full prose: what it does, when it is the right choice, what it assumes
    /// and where it stops being valid. This is what a user reads instead of the
    /// source when deciding whether the algorithm suits their data.
    std::string description;
    /// JSON array of citation objects, so a user knows what to cite. Fields:
    /// type, authors, title, year, and where applicable journal, volume,
    /// pages, doi, url.
    std::string references_json;

    // -- classification --
    std::string capability;       ///< "burst_search", "decay_fit", "fcs", ...

    /// The method a caller invokes to run this, when one exists as an attribute
    /// on the object -- emitted as `method`, which is the key the burst_search
    /// and fit categories have always carried and which a UI dispatches on. An
    /// algorithm reachable only by name (a plugin's) leaves this empty, and
    /// *that absence is the signal* those consumers already read.
    std::string dispatch_name;
    /// "builtin" unless a plugin supplied it. Emitted as `provider`.
    std::string provider = "builtin";

    // -- contract --
    std::string settings_schema;  ///< JSON Schema of the parameters
    std::string inputs_json;      ///< required/optional inputs
    std::string outputs_json;     ///< produced columns (mmfdb items)
    std::string row_grain;        ///< "burst", "curve_point", "photon", ...
    bool can_replay = false;      ///< re-executable from settings + inputs

    // -- capability-specific keys --
    /// A JSON object whose keys are merged into the emitted entry *after* the
    /// generic ones, so a capability can carry what only it needs -- a fit's
    /// `params_schema` / `results_schema` / `setup` link / `n_patterns` /
    /// `supports_lnprob`, an operation's `data_format` / `kind` / structured
    /// `inputs` -- without the descriptor growing a field per capability. Key
    /// order is preserved (ordered_json), which is what the fit parameter
    /// flattening rule depends on. Empty means nothing extra.
    std::string extra_json;

    // -- dispatch --
    void* impl = nullptr;         ///< capability-specific, opaque here
};

/// The registry key of \p d: `name`, or `operation_type` when `name` is empty.
inline const std::string& algorithm_key(const AlgorithmDescriptor& d) {
    return d.name.empty() ? d.operation_type : d.name;
}

/*!
 * \brief The table registrations go into, and the JSON it serves.
 *
 * \tparam Json an nlohmann-compatible JSON type; `nlohmann::ordered_json` keeps key order.
 *
 * Thread-safe. A registration is never replaced or removed, so a descriptor found once stays where
 * it was.
 */
template <class Json>
class AlgorithmTable {
 public:
    /*!
     * \brief Register one algorithm.
     *
     * \return false if the descriptor is incomplete (no operation_type or no
     *         capability) or if its key (`name`, else `operation_type`) is already
     *         registered. A duplicate is rejected rather than overwritten: two
     *         algorithms answering to one name is not a preference the registry can
     *         resolve, and silently keeping the last one registered makes the result
     *         depend on load order.
     */
    bool add(const AlgorithmDescriptor& desc) {
        if (desc.operation_type.empty() || desc.capability.empty()) return false;
        std::lock_guard<std::mutex> lock(m_);
        const std::string& key = algorithm_key(desc);
        if (by_name_.find(key) != by_name_.end()) return false;
        by_name_.emplace(key, order_.size());
        order_.push_back(desc);
        return true;
    }

    /*!
     * \brief Register one algorithm from a complete JSON entry.
     *
     * The entry is what the registry will show under \p capability / \p key: the
     * generic descriptor fields are read from it (`label`, `summary`,
     * `description`, `operation_type` (defaults to \p key), `params_schema` /
     * `settings_schema`, `inputs`, `outputs`, `row_grain`, `can_replay`,
     * `method`, `provider`) and the whole object is carried as `extra_json`, so
     * every key the entry had is emitted back exactly.
     *
     * \return as add(); also false if \p entry_json is not a JSON object.
     */
    bool add_json(const std::string& capability, const std::string& key, const std::string& entry_json,
                  void* impl = nullptr) {
        Json e = Json::parse(entry_json, nullptr, false);
        if (e.is_discarded() || !e.is_object()) return false;
        auto str = [&](const char* k) -> std::string {
            auto it = e.find(k);
            return (it != e.end() && it->is_string()) ? it->template get<std::string>() : std::string();
        };
        auto obj = [&](const char* k) -> std::string {
            auto it = e.find(k);
            return it != e.end() ? it->dump() : std::string();
        };
        AlgorithmDescriptor d;
        d.capability = capability;
        d.name = key;
        d.operation_type = str("operation_type").empty() ? key : str("operation_type");
        d.display_name = str("label");
        d.summary = str("summary");
        d.description = str("description");
        d.dispatch_name = str("method");
        d.provider = str("provider").empty() ? std::string("builtin") : str("provider");
        d.settings_schema = e.contains("settings_schema") ? obj("settings_schema") : obj("params_schema");
        d.inputs_json = obj("inputs");
        d.outputs_json = obj("outputs");
        d.row_grain = str("row_grain");
        d.references_json = obj("references");
        auto cr = e.find("can_replay");
        d.can_replay = cr != e.end() && cr->is_boolean() && cr->template get<bool>();
        d.extra_json = entry_json;
        d.impl = impl;
        return add(d);
    }

    /// Every registration of one capability, `{key: entry}`, in registration order.
    Json entries(const std::string& capability) const {
        std::lock_guard<std::mutex> lock(m_);
        Json out = Json::object();
        for (const auto& d : order_)
            if (d.capability == capability) out[algorithm_key(d)] = entry_of(d);
        return out;
    }

    /// The capabilities that have at least one registration, in registration order.
    std::vector<std::string> capabilities() const {
        std::lock_guard<std::mutex> lock(m_);
        std::vector<std::string> out;
        for (const auto& d : order_)
            if (std::find(out.begin(), out.end(), d.capability) == out.end()) out.push_back(d.capability);
        return out;
    }

    /// The descriptor registered under \p key, or nullptr. The pointer stays valid.
    const AlgorithmDescriptor* find(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_);
        auto it = by_name_.find(key);
        if (it == by_name_.end()) return nullptr;
        return &order_[it->second];
    }

    /// Every key registered under \p capability, in registration order.
    std::vector<std::string> keys(const std::string& capability) const {
        std::lock_guard<std::mutex> lock(m_);
        std::vector<std::string> out;
        for (const auto& d : order_)
            if (d.capability == capability) out.push_back(algorithm_key(d));
        return out;
    }

    /// The `can_replay` registrations, each with `kind` (defaulting to its capability).
    Json replayable() const {
        std::lock_guard<std::mutex> lock(m_);
        Json out = Json::object();
        for (const auto& d : order_) {
            if (!d.can_replay) continue;
            Json e = entry_of(d);
            if (!e.contains("kind")) e["kind"] = d.capability;
            out[algorithm_key(d)] = e;
        }
        return out;
    }

    /*!
     * \brief Every capability as `{capability: {key: entry}}`.
     *
     * The capabilities named in \p leading come first, in that order; every other one follows
     * alphabetically -- not in registration order, which is link order and differs between
     * bindings. A capability without registrations is left out.
     */
    Json categories(const std::vector<std::string>& leading = {}) const {
        std::vector<std::string> order = leading;
        std::vector<std::string> rest;
        for (const std::string& c : capabilities())
            if (std::find(order.begin(), order.end(), c) == order.end()) rest.push_back(c);
        std::sort(rest.begin(), rest.end());
        order.insert(order.end(), rest.begin(), rest.end());
        Json root = Json::object();
        for (const std::string& c : order) {
            Json e = entries(c);
            if (!e.empty()) root[c] = e;
        }
        return root;
    }

    /// Parse a JSON field, or fall back. A descriptor written by hand -- or by a
    /// plugin author who is not obliged to be careful -- can carry a malformed
    /// schema; that must degrade to an empty object in the registry rather than
    /// throw out of the registry and take the whole library's introspection with it.
    static Json parse_or(const std::string& text, Json fallback) {
        if (text.empty()) return fallback;
        Json v = Json::parse(text, nullptr, false);
        if (v.is_discarded()) return fallback;
        return v;
    }

    /// The entry a registration emits.
    static Json entry_of(const AlgorithmDescriptor& d) {
        // Key order matters only for readability, but the *set* of keys is a
        // compatibility surface: the burst_search and fit categories predate the
        // descriptor and their consumers (ChiSurf, ndx, the web UI) read `method`,
        // `params_schema` and `provider`. Those are emitted under their original
        // names so a category can migrate onto registrations without its entries
        // changing shape. Everything else is additive, which a consumer ignores.
        Json e = Json::object();
        e["name"] = algorithm_key(d);
        e["label"] = d.display_name.empty() ? d.operation_type : d.display_name;
        // Emitted only when there is one. Its ABSENCE is what routes a plugin's
        // search through the by-name path in every consumer that reads this, so an
        // empty string here would be a behaviour change, not a cosmetic one.
        if (!d.dispatch_name.empty()) e["method"] = d.dispatch_name;
        e["summary"] = d.summary;
        e["description"] = d.description;
        const Json schema = parse_or(d.settings_schema, Json::object());
        e["params_schema"] = schema;      // the name these categories have always used
        e["settings_schema"] = schema;    // the descriptor's own name for it
        e["capability"] = d.capability;
        e["operation_type"] = d.operation_type;
        e["row_grain"] = d.row_grain;
        e["inputs"] = parse_or(d.inputs_json, Json::object());
        e["outputs"] = parse_or(d.outputs_json, Json::object());
        e["references"] = parse_or(d.references_json, Json::array());
        e["provider"] = d.provider.empty() ? std::string("builtin") : d.provider;
        e["can_replay"] = d.can_replay;
        // Capability-specific keys last, so they win over the generic spelling of
        // the same key (a fit's `params_schema` is its own, not `settings_schema`).
        const Json extra = parse_or(d.extra_json, Json::object());
        if (extra.is_object())
            for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
        return e;
    }

 private:
    mutable std::mutex m_;
    std::deque<AlgorithmDescriptor> order_;                 // registration order; a deque keeps addresses
    std::unordered_map<std::string, std::size_t> by_name_;
};

}  // namespace tttrlib

#endif  // TTTRLIB_REGISTRYCORE_H
