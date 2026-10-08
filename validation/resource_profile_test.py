"""Resource evidence must not invent zeros or double-count overlapping scopes."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("report", Path(__file__).resolve().parents[1] / "research/resource_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class ResourceEvidenceTest(unittest.TestCase):
    def test_docker_desktop_terminal_redraw_does_not_drop_samples(self):
        sample = report.parse_sample('\x1b[J\x1b[H{"Name":"api","CPUPerc":"2.5%"}\x1b[K\n')
        self.assertEqual(sample["CPUPerc"], "2.5%")

    def test_missing_and_stale_components_are_gaps_in_totals(self):
        sample = lambda cpu, ram: {"seconds": 1, "sample": {"CPUPerc": cpu, "MemUsage": ram}}
        data = report.series({"samples": [
            {"seconds": 1, "containers": {"api": sample("10%", "1MiB / 64MiB"), "replay": sample("110%", "2MiB / 6GiB")}},
            {"seconds": 2, "containers": {"api": sample("10%", "1MiB / 64MiB")}},
            {"seconds": 8, "containers": {"api": sample("10%", "1MiB / 64MiB"), "replay": sample("110%", "2MiB / 6GiB")}}]})
        self.assertEqual(data["All containers"][0], [1, 120, 3])
        self.assertIsNone(data["All containers"][1][1])
        self.assertIsNone(data["All containers"][2][2])

    def test_host_and_browser_are_not_added_to_containers(self):
        data = report.series({"samples": [], "host_samples": [{"seconds": 1,
            "sample": {"cpu_percent": 50, "ram_bytes": 1024**3, "browser_ram_bytes": 1024**2}}]})
        self.assertEqual(data["All containers"], [])
        self.assertEqual(data["Windows host"][0][2], 1024)
        self.assertEqual(data["All browser processes"][0][2], 1)

    def test_report_retains_failure_and_escapes_script_payload(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            report.write_report(path, {"result": "FAIL", "days": 100, "requested_seconds": 30,
                "errors": ["<unsafe>"], "samples": [{"seconds": 1, "containers": {
                "</script>": {"seconds": 1, "sample": {"CPUPerc": "0%", "MemUsage": "1MB / 6GB"}}}}]})
            page = (path / "resources.html").read_text()
            self.assertIn("FAIL", page)
            self.assertIn("&lt;unsafe&gt;", page)
            self.assertIn("\\u003c/script>", page)
            self.assertEqual(report.memory_bytes("1MB / 6GB"), 1000000)


if __name__ == "__main__":
    unittest.main()
