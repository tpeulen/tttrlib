# SPDX-License-Identifier: BSD-3-Clause
"""The vendored ptolib source package stays identical to its manifest and upstream.

Refresh with tools/sync_ptolib.sh (ptolib's scripts/vendor.sh, which copies the
buildable source package and writes VENDORING.json). The manifest check also runs
in a standalone checkout; comparing with upstream needs a sibling ptolib checkout
or PTOLIB_CHECKOUT.
"""
import hashlib
import json
import os
import unittest


def _sha(path):
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read()).hexdigest()


class TestVendoredPtolib(unittest.TestCase):
    def _ptolib_paths(self):
        repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        return (os.path.join(repo, "thirdparty", "ptolib"),
                os.environ.get("PTOLIB_CHECKOUT", os.path.join(os.path.dirname(repo), "ptolib")))

    def test_ptolib_manifest_matches_vendored_sources(self):
        """Validate every vendored source even without a sibling checkout."""
        vendor = self._ptolib_paths()[0]
        with open(os.path.join(vendor, "VENDORING.json")) as fh:
            manifest = json.load(fh)
        self.assertIn("CMakeLists.txt", manifest)
        self.assertIn("include/ptolib/ptolib.h", manifest)
        self.assertIn("src/ptolib.cpp", manifest)
        for rel, expected in manifest.items():
            self.assertFalse(os.path.isabs(rel), rel)
            self.assertNotIn("..", rel.split("/"), rel)
            path = os.path.join(vendor, rel)
            self.assertTrue(os.path.isfile(path), rel)
            self.assertFalse(os.path.islink(path), rel)
            self.assertEqual(_sha(path), expected, rel + ": refresh ptolib")

    def test_ptolib_sources_match_checkout_when_present(self):
        vendor, checkout = self._ptolib_paths()
        if not os.path.isdir(os.path.join(checkout, "include", "ptolib")):
            self.skipTest("ptolib checkout not present; cannot compare")
        with open(os.path.join(vendor, "VENDORING.json")) as fh:
            manifest = json.load(fh)
        for rel in manifest:
            self.assertEqual(_sha(os.path.join(vendor, rel)),
                             _sha(os.path.join(checkout, rel)),
                             rel + ": refresh the ptolib source package")


if __name__ == "__main__":
    unittest.main()
