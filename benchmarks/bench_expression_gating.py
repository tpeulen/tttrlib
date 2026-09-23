"""Gating a DataStore: what a query costs on the ndxplorer and PTO paths.

These are the two workloads that matter for the expression engine, named
because they are the ones users wait on:

- **ndx** -- ndxplorer holds burst parameters in a `DataStore` as **float32**
  (`ndxplorer/core/data_source.py:build_store`) and gates them with
  `DataSource.query_mask`, which calls `DataStore.select_expression`. The
  queries below are the ones ndxplorer's own test suite uses, so the shapes
  are real rather than invented.
- **pto** -- a `DataStore` is written into and read back out of a PTO file
  (`pto_add_store` / `pto_read_store`), and then gated. Same evaluator; this
  measures it on a store that came off disk.

Baseline for the port of the block-vectorised engine out of imp.bff. Run
before and after; the numbers only mean something as a pair.

    python benchmarks/bench_expression_gating.py

Timings are min-of-N. Check the load average printed at the top -- above
about 2 the numbers are a floor, not a measurement.
"""

import os
import tempfile
import time

import numpy as np

import tttrlib

REPEATS = 7
ROW_COUNTS = [100_000, 1_000_000, 5_000_000]

# ndxplorer's own test queries, verbatim from
# chisurf/modules/ndxplorer/ndxplorer/tests/test_bff_query.py
QUERIES = [
    "(g>2) & (r<10)",
    "g>5 | b<0.1",
    "~(g>2) & (r>1)",
    "(g-b)/(r-b) > 0.3",
    "g>2 and r<10",
    "g != 3",
]


def best(fn):
    times = []
    for _ in range(REPEATS):
        t0 = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t0)
    return min(times) * 1e3


def make_store(n, seed=0, dtype=np.float32):
    """A store shaped like ndxplorer's: named numeric columns, float32."""
    rng = np.random.default_rng(seed)
    store = tttrlib.DataStore()
    store.add("g", rng.uniform(0, 10, n).astype(dtype))
    store.add("r", rng.uniform(0, 20, n).astype(dtype))
    store.add("b", rng.uniform(0, 1, n).astype(dtype))
    store.set_n_rows(int(n))
    return store


def frame_of(store, n, seed=0):
    import pandas as pd
    rng = np.random.default_rng(seed)
    return pd.DataFrame({"g": rng.uniform(0, 10, n).astype(np.float32),
                         "r": rng.uniform(0, 20, n).astype(np.float32),
                         "b": rng.uniform(0, 1, n).astype(np.float32)})


def report(title, rows):
    print(f"\n{title}")
    header = f"{'query':<22}{'rows':>10}{'count_expr':>12}{'pandas':>10}{'vs pd':>8}"
    print(header)
    print("-" * len(header))
    for r in rows:
        print(r)


def roundtrip_through_pto(store, path):
    """The same store, written into a PTO file and read back out of it.

    A store that came off disk is the one a user actually gates: the columns
    were built by the reader rather than by numpy, and nothing about the
    evaluator should notice. Measuring it is the only way to know that.
    """
    f = tttrlib.PtoFile()
    if not f.create(path, "bench"):
        raise RuntimeError(f.error())
    uid = tttrlib.pto_add_store(f, "table", "bursts", store)
    if not f.commit():
        raise RuntimeError(f.error())
    f.close()

    g = tttrlib.PtoFile()
    if not g.open(path):
        raise RuntimeError(g.error())
    back = tttrlib.DataStore()
    tttrlib.pto_read_store(g, uid, back)
    return back


def main():
    print(f"tttrlib {tttrlib.__version__}   load average "
          f"{os.getloadavg()[0]:.1f}   min of {REPEATS}   times in ms")

    rows = []
    for n in ROW_COUNTS:
        store = make_store(n)
        frame = frame_of(store, n)
        for q in QUERIES:
            # Correctness before speed: the store and pandas must agree.
            store.clear_row_mask()
            got = int(store.count_expression(q))
            want = int(np.asarray(frame.eval(q)).sum())
            flag = "" if got == want else f"  MISMATCH got={got} want={want}"

            t_store = best(lambda: store.count_expression(q))
            t_pandas = best(lambda: frame.eval(q))
            rows.append(f"{q:<22}{n:>10,}{t_store:>12.3f}{t_pandas:>10.3f}"
                        f"{t_pandas / t_store:>7.2f}x{flag}")
    report("ndx: float32 columns, gated in place", rows)

    # float64, to show what the non-float32 path costs (it widens every
    # referenced column into a scratch buffer before evaluating).
    rows = []
    for n in [1_000_000]:
        store = make_store(n, dtype=np.float64)
        frame = frame_of(store, n)
        for q in QUERIES:
            t_store = best(lambda: store.count_expression(q))
            t_pandas = best(lambda: frame.eval(q))
            rows.append(f"{q:<22}{n:>10,}{t_store:>12.3f}{t_pandas:>10.3f}"
                        f"{t_pandas / t_store:>7.2f}x")
    report("float64 columns (the widening path)", rows)

    # pto: the same gate on a store that came off disk rather than out of numpy.
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for n in [1_000_000]:
            store = make_store(n)
            frame = frame_of(store, n)
            back = roundtrip_through_pto(store, os.path.join(tmp, "bench.pto"))
            for q in QUERIES:
                back.clear_row_mask()
                got = int(back.count_expression(q))
                want = int(np.asarray(frame.eval(q)).sum())
                flag = "" if got == want else f"  MISMATCH got={got} want={want}"
                t_store = best(lambda: back.count_expression(q))
                t_pandas = best(lambda: frame.eval(q))
                rows.append(f"{q:<22}{n:>10,}{t_store:>12.3f}{t_pandas:>10.3f}"
                            f"{t_pandas / t_store:>7.2f}x{flag}")
    report("pto: written to a file, read back, then gated", rows)


if __name__ == "__main__":
    main()
