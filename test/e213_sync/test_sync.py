"""Safety checks for explicit scene links and three-way manuscript sync."""
import importlib.util
import pathlib
import unittest

TARGET = pathlib.Path(__file__).resolve().parents[2] / "tools/e213-sync/plot_jot_sync.py"
spec = importlib.util.spec_from_file_location("e213_plot_jot_sync", TARGET)
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)

def h(text):
    return sync.checksum(text.encode("utf-8"))


class DecisionTests(unittest.TestCase):
    def test_only_e213_changed(self):
        self.assertEqual(sync.decide_sync(h("original"), h("edited on E213"), h("original")), "push")

    def test_only_desktop_changed(self):
        self.assertEqual(sync.decide_sync(h("original"), h("original"), h("edited on PC")), "pull")

    def test_both_changed_creates_conflict(self):
        self.assertEqual(sync.decide_sync(h("original"), h("device"), h("PC")), "conflict")

    def test_same_content_requires_no_write(self):
        self.assertEqual(sync.decide_sync(h("old"), h("new"), h("new")), "same")

    def test_unknown_revision_is_rejected(self):
        with self.assertRaises(ValueError):
            sync.decide_sync("bad", h("new"), h("other"))


class ManifestTests(unittest.TestCase):
    def manifest(self):
        return {
            "version": 1, "provider": "plot-and-jot", "project_id": 42,
            "project_name": "Test", "scenes": {
                "Draft-001.txt": {"scene_id": 3, "chapter_id": 4, "revision": h("draft")}
            }
        }

    def test_valid_binding(self):
        sync.validate_manifest(self.manifest(), 42)

    def test_rejects_wrong_project(self):
        with self.assertRaisesRegex(ValueError, "another project"):
            sync.validate_manifest(self.manifest(), 43)

    def test_rejects_traversal(self):
        m = self.manifest()
        m["scenes"]["../escape.txt"] = m["scenes"].pop("Draft-001.txt")
        with self.assertRaisesRegex(ValueError, "Unsafe"):
            sync.validate_manifest(m, 42)

    def test_rejects_missing_revision(self):
        m = self.manifest()
        m["scenes"]["Draft-001.txt"]["revision"] = ""
        with self.assertRaisesRegex(ValueError, "Invalid binding"):
            sync.validate_manifest(m, 42)

    def test_rejects_non_numeric_ids(self):
        m = self.manifest()
        m["scenes"]["Draft-001.txt"]["scene_id"] = "3"
        with self.assertRaises(ValueError):
            sync.validate_manifest(m, 42)


if __name__ == "__main__":
    unittest.main()
