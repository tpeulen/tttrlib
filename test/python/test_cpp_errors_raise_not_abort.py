"""A C++ throw must reach Python as an exception, never terminate the process.

SWIG's `%exception` is positional: it applies to everything declared after it
until replaced or cleared, and a bare `%exception;` clears it to *nothing*
rather than restoring what was there before. So one interface file ending with
a bare clear silently disarms every interface included after it, and a C++
throw from any of them aborts the interpreter:

    libc++abi: terminating due to uncaught exception of type
    std::invalid_argument: TAC2 needs at least two frames

That killed the whole test suite mid-run (BUGS 2026-08-11) — not a failure, a
`Fatal Python error: Aborted`, so nothing after it ran either. The subsystems
below sit after the last such clear in the include order and are the ones that
lose their handler first; each call here throws in C++ on purpose.
"""

import pathlib
import re
import unittest

import numpy as np

import tttrlib


class TestCppErrorsRaiseRatherThanAbort(unittest.TestCase):
    """Each case would abort the interpreter, not fail, if the handler is lost."""

    def test_superres_temporal_combine_too_few_frames(self):
        with self.assertRaises(Exception):
            tttrlib.CLSMSuperRes.temporal_combine(np.zeros((1, 4, 4)), mode="TAC2")

    def test_superres_temporal_combine_wrong_rank(self):
        with self.assertRaises(Exception):
            tttrlib.CLSMSuperRes.temporal_combine(np.zeros((4, 4)), mode="AVG")

    def test_superres_temporal_combine_unknown_mode(self):
        with self.assertRaises(Exception):
            tttrlib.CLSMSuperRes.temporal_combine(np.zeros((2, 4, 4)), mode="NOPE")

    def test_an_unidentifiable_file_raises(self):
        # TTTR is wrapped in the same after-the-clear region.
        with self.assertRaises(Exception):
            tttrlib.TTTR("/nonexistent/path/definitely_not_a_photon_file.xyz")

    def test_tsne_perplexity_above_n_samples_is_a_value_error(self):
        # Embedding.i sat after the bare clears in NdImage.i and Signal.i.
        with self.assertRaises(ValueError):
            tttrlib.tsne(np.random.default_rng(0).normal(size=(50, 3)), perplexity=60)


class TestNoInterfaceClearsTheGlobalHandler(unittest.TestCase):
    """The runtime cases above only probe a few subsystems; this probes all."""

    def test_no_python_interface_ends_with_a_bare_clear(self):
        root = pathlib.Path(__file__).resolve().parents[2] / "ext" / "python"
        if not root.is_dir():
            self.skipTest("interface sources are not beside the tests")
        offenders = [
            f"{path.relative_to(root)}:{n}"
            for path in sorted(root.rglob("*.i"))
            for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1)
            if re.fullmatch(r"\s*%exception\s*;\s*", line)
        ]
        self.assertEqual(offenders, [], "restore the standard %exception handler instead of `%exception;`")


if __name__ == "__main__":
    unittest.main()
