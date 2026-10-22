"""Resource evidence must not invent zeros or double-count overlapping scopes."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import yaml

spec = importlib.util.spec_from_file_location("report", Path(__file__).resolve().parents[1] / "research/resource_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

spec = importlib.util.spec_from_file_location("profile", Path(__file__).resolve().parents[1] / "research/resource_profile.py")
profile = importlib.util.module_from_spec(spec)
with mock.patch.dict(sys.modules, {"resource_report": report}):
    spec.loader.exec_module(profile)


class ResourceTopologyTest(unittest.TestCase):
    def test_dashboard_uses_isolated_retained_fixtures_without_docker(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            replay_dir = path / "replay"
            replay_dir.mkdir()
            runner = path / "runner"
            compose = path / "compose.yml"
            compose.write_text(yaml.safe_dump({"services": {}, "volumes": {"market-data-db": {}},
                                               "networks": {"historical-replay": {"internal": True}}}))
            sandbox = SimpleNamespace(path=path, project="profile-test", manifest={"image": "fixture", "files": {}})
            campaign = SimpleNamespace(ROOT=root, command=mock.Mock(return_value="fixture-image"),
                                       digest=lambda _: "fixture-digest", Sandbox=lambda _: sandbox)
            result = profile.install_dashboard(sandbox, campaign, replay_dir, runner,
                                               [str(runner), "--output", str(replay_dir / "summary.json")])
            self.assertIs(result, sandbox)
            topology = yaml.safe_load(compose.read_text())
            services = topology["services"]
            self.assertEqual(set(services), {"dashboard-api", "dashboard-web", "dashboard-watchdog",
                                             "dashboard-alert-notifier", "profile-replay"})
            api = services["dashboard-api"]
            self.assertEqual(api["environment"]["DASHBOARD_DATA_PROVIDER"], "simulation")
            self.assertEqual(api["volumes"], [str(replay_dir / "simulation") + ":/data/simulation:ro"])
            self.assertEqual(api["environment"]["DASHBOARD_AUTH_ALLOW_DEMO"], "true")
            self.assertEqual(services["dashboard-web"]["ports"], ["127.0.0.1::80"])
            watchdog = services["dashboard-watchdog"]
            self.assertEqual(watchdog["entrypoint"], ["/dashboard-watchdog"])
            self.assertEqual(watchdog["environment"]["DASHBOARD_RUNTIME_MODE"], "PAPER")
            self.assertEqual(watchdog["environment"]["DASHBOARD_EXECUTION_VENUE"], "SIMULATED")
            notifier = services["dashboard-alert-notifier"]
            self.assertEqual(notifier["entrypoint"], ["/dashboard-alert-notifier"])
            self.assertEqual(notifier["environment"]["DASHBOARD_NOTIFIER_SINK"], "TEST_FILE")
            self.assertEqual(notifier["volumes"], ["dashboard-watchdog-data:/data/watchdog:ro",
                                                    "dashboard-alert-notifier-data:/data/notifier"])
            self.assertNotIn("TELEGRAM", json.dumps(notifier["environment"]))
            for name, service in services.items():
                self.assertNotIn("container_name", service)
                self.assertNotIn("build", service)
                self.assertNotIn("network_mode", service)
                self.assertTrue(set(service["networks"]).issubset(topology["networks"]))
                for volume in service.get("volumes", []):
                    source = volume.split(":")[0]
                    self.assertTrue(source in topology["volumes"] or source.startswith(str(path))
                                    or source.startswith(str(root / "deploy/historical_replay/run"))
                                    or source.startswith(str(root / "config/historical_replay")), (name, source))
            for volume in topology["volumes"].values():
                if volume:
                    self.assertTrue(volume["name"].startswith(sandbox.project + "-"))


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
