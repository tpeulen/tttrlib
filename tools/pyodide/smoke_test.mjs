// Smoke test for the Pyodide wheel, run under Node.
//
//   node tools/pyodide/smoke_test.mjs <wheel> [data-dir]
//
// Needs the npm `pyodide` package at the version the wheel was built for. It is
// resolved from PYODIDE_NODE_DIR (default ~/opt/pyodide-node, a directory with
// `npm install pyodide@0.28.0` done in it) so node_modules stays out of the
// checkout. numpy comes from the Pyodide CDN, so the first run needs network.
//
// data-dir defaults to ~/dev/tttr-data and is mounted read-only-in-spirit at
// /data through NODEFS. It exercises what ndXplorer needs from tttrlib in the
// browser: a DataStore from a burst table, a photon stream from a .pto, and the
// accurate-FRET calibration (against its native numbers).
import { createRequire } from "node:module";
import { homedir } from "node:os";
import path from "node:path";

const [wheel, dataDir = path.join(homedir(), "dev", "tttr-data")] = process.argv.slice(2);
if (!wheel) {
  console.error("usage: node smoke_test.mjs <wheel> [data-dir]");
  process.exit(2);
}
const nodeDir = process.env.PYODIDE_NODE_DIR || path.join(homedir(), "opt", "pyodide-node");
const require = createRequire(path.join(nodeDir, "package.json"));
const { loadPyodide } = require("pyodide");

const pyodide = await loadPyodide();
console.log(`pyodide ${pyodide.version}`);
await pyodide.loadPackage("numpy");

pyodide.FS.mkdirTree("/data");
pyodide.FS.mount(pyodide.FS.filesystems.NODEFS, { root: path.resolve(dataDir) }, "/data");
// under Node, loadPackage reads a local path straight from disk
await pyodide.loadPackage(path.resolve(wheel));

const script = String.raw`
import os, traceback
import numpy as np
import tttrlib

print("tttrlib", tttrlib.__version__, "numpy", np.__version__)
ok = True

def check(label, fn):
    global ok
    try:
        print(f"[ok]   {label}: {fn()}")
    except Exception:
        ok = False
        print(f"[FAIL] {label}")
        traceback.print_exc()

bur = "/data/bh/bh_spc132_sm_dna/sliding_window_All 0.1500#60/bi4_bur/m000.bur"
pto = "/data/bh/bh_spc132_sm_dna/m000.pto"

def datastore_roundtrip():
    s = tttrlib.DataStore()
    s.add("x", np.arange(5, dtype=np.float64))
    return list(np.asarray(s["x"]))

def burst_table():
    store = tttrlib.read_csv(bur, delimiter="\t", has_header=True)
    names = list(store.names)
    col = np.asarray(store["Number of Photons"])
    return f"{len(store)} rows x {len(names)} cols; 'Number of Photons'[:5]={col[:5].tolist()}"

def photon_stream():
    t = tttrlib.TTTR(pto)
    mt = np.asarray(t.macro_times)
    ch = np.asarray(t.routing_channels)
    return f"{len(t)} photons, macro_times[:3]={mt[:3].tolist()}, channels={sorted(set(ch[:10000].tolist()))}"

check("DataStore in memory", datastore_roundtrip)
check("DataStore from .bur (read_csv)", burst_table)
check("TTTR from .pto", photon_stream)

def burst_histogram():
    store = tttrlib.read_csv(bur, delimiter="\t", has_header=True)
    h = store.histogram("Duration (ms)", bins=16)
    return type(h).__name__

check("DataStore.histogram", burst_histogram)

def correlation():
    # the first lags hold 64-bit macro-time offsets: they read 0.0 when a
    # size_t (32 bit on wasm32) truncates them; natively 1.3702, 1.8684
    t = tttrlib.TTTR(pto)
    c = tttrlib.Correlator(tttr=t, channels=([0, 8], [1, 9]), n_bins=5, n_casc=20)
    g = np.round(np.asarray(c.correlation), 4)
    assert g[1] == 1.3702 and g[2] == 1.8684, g[:3]
    return f"g[1:3]={g[1:3].tolist()} g[-1]={g[-1]}"

check("Correlator (64-bit macro times)", correlation)

def afret_bursts():
    # synthetic ALEX bursts (numpy's PCG64 draws the same numbers in both builds)
    rng = np.random.default_rng(7)
    def pop(n, e, g=0.8, b=0.9, a=0.07, d=0.05, kind="fret"):
        tot = rng.uniform(40, 160, n)
        if kind == "donor":
            return rng.poisson(tot), rng.poisson(a * tot), rng.poisson(0.3, n)
        if kind == "acceptor":
            return rng.poisson(0.3, n), rng.poisson(d * tot * b), rng.poisson(tot * b)
        fdd, fda, faa = tot * (1 - e) / g, tot * e, tot * b
        return rng.poisson(fdd), rng.poisson(fda + a * fdd + d * faa), rng.poisson(faa)
    parts = [pop(300, 0, kind="donor"), pop(300, 0, kind="acceptor"), pop(800, 0.3), pop(800, 0.7)]
    return [np.concatenate([p[i] for p in parts]).astype(float) for i in range(3)]

def accurate_fret():
    # the factors and bootstrap sigmas are the native arm64 values
    dd, da, aa = afret_bursts()
    r = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa}, None,
                               {"n_bootstrap": 20, "seed": 3})
    keys = ("alpha", "delta", "gamma", "beta")
    native_f = [0.07072758037225042, 0.046164031213457225, 0.7900987503221061, 0.9027118931557756]
    native_u = [0.001378009122758327, 0.0011193468843084561, 0.010835690353169598, 0.007684365811760359]
    np.testing.assert_allclose([r["factors"][k] for k in keys], native_f, rtol=1e-9)
    np.testing.assert_allclose([r["uncertainties"][k] for k in keys], native_u, rtol=1e-9)
    assert r["species"]["model_selection"]["selected"] == "shared"
    acc = tttrlib.accurate_fret(dd, da, aa, factors=r["factors"], uncertainties=r["uncertainties"])
    return (f"gamma={r['factors']['gamma']:.6f} beta={r['factors']['beta']:.6f} "
            f"populations={r['split']['counts']['fret_populations']} "
            f"R={acc['populations'][0]['distance']:.2f}")

check("accurate FRET auto_calibrate (equals native)", accurate_fret)

def accurate_fret_hdbscan():
    # density-based populations over S and E (HDBSCAN from modules/math); the
    # factors are the native arm64 values
    dd, da, aa = afret_bursts()
    r = tttrlib.auto_calibrate({"i_dd": dd, "i_da": da, "i_aa": aa}, None, {"dimensions": ["S", "E"]})
    keys = ("alpha", "delta", "gamma", "beta")
    native_f = [0.0712005003456335, 0.04733165397218232, 0.7968978322745337, 0.899002539542906]
    np.testing.assert_allclose([r["factors"][k] for k in keys], native_f, rtol=1e-9)
    c = r["split"]["counts"]
    assert r["split"]["method"] == "hdbscan" and c["fret_populations"] == 2 and c["noise"] == 15, c
    return f"gamma={r['factors']['gamma']:.6f} populations={c['fret_populations']} noise={c['noise']}"

check("accurate FRET HDBSCAN populations (equals native)", accurate_fret_hdbscan)
ok
`;
const ok = await pyodide.runPythonAsync(script);
process.exit(ok ? 0 : 1);
