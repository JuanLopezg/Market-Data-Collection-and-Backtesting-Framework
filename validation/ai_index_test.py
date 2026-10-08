#!/usr/bin/env python3
"""Navigation safety and freshness fixtures; run under WSL."""

import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.generate_ai_index import build_index, generate


class RepositoryIndexTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ai-index-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.git("init", "--quiet")
        self.write(".gitignore", ".ai/\nstorage/\nbuild/\n*.env\nignored.py\n")
        self.write("AGENTS.md", "Read current source.\n")
        self.write("lib/src/execution/engine.h", "struct Engine {};\n")
        self.write("lib/src/execution/engine.cpp", '#include "engine.h"\n#include "ambiguous.h"\n')
        self.write("lib/src/a/ambiguous.h", "struct A {};\n")
        self.write("lib/src/b/ambiguous.h", "struct B {};\n")
        self.write("research/src/legacy/meson.build", "executable(\n  'study', ['study.cpp']\n)\n")
        self.write("research/src/legacy/study.cpp", '#include "engine.h"\n')
        self.git("add", ".")

    def git(self, *arguments):
        subprocess.run(["git", *arguments], cwd=self.root, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def refresh(self, check=False):
        with contextlib.redirect_stdout(io.StringIO()):
            return generate(self.root, check)

    def test_ignored_tracked_credentials_runtime_and_links_are_excluded(self):
        forbidden = ["config/credentials.json", "config/.env.example", "config/private.key",
                     "storage/report.txt", "build/cache.py", "tools/ignored.py",
                     "docs/venue/step1/frozen.md", "tools/historical_replay/old.py",
                     "deploy/historical_replay/run/result.json", "lib/src/strategy/on_hold/old.h",
                     "research/src/legacy/runtime/old.h"]
        marker = "PRIVATE_FIXTURE_VALUE"
        for path in forbidden:
            self.write(path, marker)
        self.git("add", "--force", *forbidden)
        outside = self.root.parent / (self.root.name + "-outside.h")
        outside.write_text(marker)
        self.addCleanup(outside.unlink)
        (self.root / "lib/src/execution/link.h").symlink_to(outside)
        linked_directory = self.root / "lib/src/linked"
        linked_directory.symlink_to(self.root / "lib/src/execution", target_is_directory=True)
        self.git("add", "lib/src/execution/link.h", "lib/src/linked")
        self.write("lib/src/execution/new.h", "struct New {};\n")
        self.write("lib/src/execution/new.h.env", marker)
        index = build_index(self.root)
        paths = {entry["path"] for entry in index["files"]}
        self.assertTrue(paths.isdisjoint(forbidden))
        self.assertNotIn("lib/src/execution/link.h", paths)
        self.assertNotIn("lib/src/linked", paths)
        self.assertIn("lib/src/execution/new.h", paths)
        self.assertNotIn(marker, json.dumps(index))
        self.assertTrue(self.refresh())
        self.assertTrue(all(marker not in p.read_text() for p in (self.root / ".ai").iterdir()))

    def test_freshness_detects_edits_additions_deletions_and_preserves_other_files(self):
        self.assertFalse(self.refresh(check=True))
        self.assertTrue(self.refresh())
        snapshot = {p.name: p.read_bytes() for p in (self.root / ".ai").iterdir()}
        self.assertTrue(self.refresh())
        self.assertEqual(snapshot, {p.name: p.read_bytes() for p in (self.root / ".ai").iterdir()})
        self.assertTrue(self.refresh(check=True))
        note = self.write(".ai/local-notes.md", "Keep this local note.\n")
        self.write("lib/src/execution/engine.cpp", '#include "engine.h"\n// changed\n')
        self.assertFalse(self.refresh(check=True))
        self.assertEqual(snapshot["index.json"], (self.root / ".ai/index.json").read_bytes())
        self.refresh()
        self.write("lib/src/execution/new.py", "print('example')\n")
        self.assertFalse(self.refresh(check=True))
        self.refresh()
        (self.root / "lib/src/execution/engine.h").unlink()
        self.assertFalse(self.refresh(check=True))
        self.refresh()
        self.write("storage/another-report.txt", "Ignored changes should not invalidate navigation.\n")
        self.assertTrue(self.refresh(check=True))
        self.assertEqual(note.read_text(), "Keep this local note.\n")

    def test_include_candidates_targets_and_portable_fingerprints(self):
        index = build_index(self.root)
        files = {entry["path"]: entry for entry in index["files"]}
        engine = files["lib/src/execution/engine.cpp"]
        self.assertEqual(engine["includes"], ["lib/src/execution/engine.h"])
        self.assertEqual(engine["include_candidates"], ["lib/src/a/ambiguous.h", "lib/src/b/ambiguous.h"])
        self.assertEqual(files["lib/src/execution/engine.h"]["included_by"],
                         ["lib/src/execution/engine.cpp", "research/src/legacy/study.cpp"])
        self.assertEqual(index["build_targets"], [{"name": "study", "kind": "executable",
                         "path": "research/src/legacy/meson.build", "line": 1}])
        for entry in index["files"]:
            path = self.root / entry["path"]
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(index, build_index(self.root))

    def test_output_symlinks_are_rejected(self):
        external = self.root / "destination"
        external.mkdir()
        (self.root / ".ai").symlink_to(external, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            self.refresh()
        (self.root / ".ai").unlink()
        self.refresh()
        (self.root / ".ai/index.json").unlink()
        (self.root / ".ai/index.json").symlink_to(self.root / "AGENTS.md")
        with self.assertRaises(RuntimeError):
            self.refresh()
        self.assertEqual((self.root / "AGENTS.md").read_text(), "Read current source.\n")


if __name__ == "__main__":
    unittest.main()
