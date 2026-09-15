// RegistryCore.h, the registry mechanism shared by tttrlib and imp.bff.
//
// Checks, each of which can fail:
// - a JSON registration emits the generic keys, its own keys win, and `method` is absent when not given;
// - a duplicate key and an incomplete descriptor are refused, and the first registration is kept;
// - `find` returns a pointer that stays valid while thousands more register (the store used to be a
//   vector, whose growth moved the descriptors);
// - `categories` puts the leading capabilities first and the rest alphabetically, and leaves out none;
// - `replayable` carries only can_replay entries, with `kind` defaulting to the capability;
// - a malformed schema degrades to an empty object instead of throwing.
//
//     c++ -std=c++17 -O2 -I modules/core/include -I thirdparty/nlohmann_json/single_include \
//         -o t test/cpp/test_registry_core.cpp && ./t
//
// Exit status is the number of failed checks. Written 2026-09-15 (imp.bff PRD-147 A2).
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>
#include "RegistryCore.h"

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    using json = nlohmann::ordered_json;
    tttrlib::AlgorithmTable<json> t;
    int token = 7;
    check(t.add_json("sampler", "nuts", R"({"label": "NUTS", "summary": "s", "params_schema": {"type": "object",
        "properties": {"max_depth": {"type": "integer", "default": 10}}}, "kind": "chain", "provider": "imp.bff"})", &token),
          "a JSON entry registers");
    const auto* d = t.find("nuts");
    check(d && d->impl == &token && d->provider == "imp.bff" && d->operation_type == "nuts", "find returns the descriptor, impl and provider");
    json e = t.entries("sampler")["nuts"];
    check(e["name"] == "nuts" && e["label"] == "NUTS" && e["kind"] == "chain" && !e.contains("method"), "generic keys, own keys, no empty method");
    check(e["params_schema"]["properties"]["max_depth"]["default"] == 10 && e["settings_schema"] == e["params_schema"], "the schema under both names");
    check(!t.add_json("sampler", "nuts", R"({"label": "other"})"), "a duplicate key is refused");
    check(t.entries("sampler")["nuts"]["label"] == "NUTS", "the first registration is kept");
    tttrlib::AlgorithmDescriptor incomplete; incomplete.name = "x";
    check(!t.add(incomplete), "a descriptor without operation_type or capability is refused");
    check(!t.add_json("sampler", "bad", "[1, 2]"), "a non-object entry is refused");
    for (int i = 0; i < 5000; ++i) t.add_json(i % 2 ? "zeta" : "alpha", "k" + std::to_string(i), R"({"can_replay": true})");
    check(d == t.find("nuts") && d->display_name == "NUTS", "a found pointer survives 5000 more registrations");
    tttrlib::AlgorithmDescriptor bad; bad.capability = "graph_node"; bad.operation_type = "g"; bad.settings_schema = "{not json";
    t.add(bad);
    const json cats = t.categories({"sampler"});
    std::string order; for (auto it = cats.begin(); it != cats.end(); ++it) order += it.key() + ",";
    check(order == "sampler,alpha,graph_node,zeta,", "categories: leading first, then alphabetical");
    const json g = t.entries("graph_node")["g"];
    check(g["params_schema"].is_object() && g["params_schema"].empty(), "a malformed schema degrades to an empty object");
    const json rep = t.replayable();
    check(rep.size() == 5000 && rep["k1"]["kind"] == "zeta" && !rep.contains("nuts"), "replayable: only can_replay, kind defaults to capability");
    check(t.keys("sampler").size() == 1 && t.capabilities().size() == 4, "keys and capabilities");
    return failures;
}
