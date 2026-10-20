# SPDX-License-Identifier: BSD-3-Clause
"""Code ported from third-party sources keeps its upstream notices, and packaging ships them.

Apache-2.0 section 4 asks a derivative to keep the upstream copyright notice, to
state prominently that the files were changed, to carry the licence text, and to
reproduce the pertinent NOTICE attribution; MIT asks for its copyright and
permission notice. The Photonscore .photons reader is such a port.
"""
import os
import re
import unittest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PORTED = ("modules/io/ps/include/PhotonscoreD7.h", "modules/io/ps/src/PhotonscoreD7.cpp")
TEXTS = ("NOTICE", "licenses/Apache-2.0.txt", "licenses/photonsfile-MIT.txt")


def _read(rel):
    with open(os.path.join(REPO, rel), encoding="utf-8") as fh:
        return fh.read()


class TestThirdPartyNotices(unittest.TestCase):
    def test_ported_files_carry_upstream_notices_and_a_change_statement(self):
        for rel in PORTED:
            head = _read(rel)[:2000]
            self.assertIn("SPDX-License-Identifier: BSD-3-Clause AND Apache-2.0 AND MIT", head, rel)
            self.assertIn("Copyright 2026 Photonscore GmbH", head, rel)
            self.assertIn("Copyright (c) 2026 Alex Hunt", head, rel)
            self.assertIn("MODIFIED by the tttrlib developers", head, rel)

    def test_licence_texts_and_notice_are_present(self):
        apache = _read("licenses/Apache-2.0.txt")
        self.assertIn("Apache License", apache[:200])
        self.assertIn("Version 2.0, January 2004", apache[:200])
        self.assertTrue(_read("licenses/photonsfile-MIT.txt").startswith("MIT License"))
        notice = _read("NOTICE")
        self.assertIn("D7 (.photons) file format reference implementation", notice)
        self.assertIn("Copyright 2026 Photonscore GmbH", notice)
        self.assertIn("modified", notice)

    def test_packaging_ships_the_notices(self):
        pyproject = _read("pyproject.toml")
        m = re.search(r"^wheel\.license-files\s*=\s*\[(.*)\]", pyproject, re.M)
        self.assertIsNotNone(m, "pyproject must list wheel.license-files")
        globs = m.group(1)
        self.assertIn('"NOTICE*"', globs)
        self.assertIn('"licenses/*"', globs)
        for recipe in ("recipes/py/recipe.yaml", "recipes/cli/recipe.yaml",
                       "recipes/r/recipe.yaml"):
            text = _read(recipe)
            for rel in ("LICENSE.txt",) + TEXTS:
                self.assertRegex(text, r"\n\s+- " + re.escape(rel) + r"\n", recipe)


if __name__ == "__main__":
    unittest.main()
