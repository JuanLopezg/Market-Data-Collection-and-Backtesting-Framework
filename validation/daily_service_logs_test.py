#!/usr/bin/env python3
"""Exercise the real host receiver, retention and merged Compose configuration under WSL."""

from datetime import datetime, timedelta, timezone
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
LOGGING = ROOT / "deploy/live/logging"
spec = importlib.util.spec_from_file_location("daily_log_cleanup", LOGGING / "clean_daily_logs.py")
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)


class DailyServiceLogsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="daily-service-logs-")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        self.logs = self.work / "algotrading/services"
        self.logs.mkdir(parents=True)

    def test_retention_calendar_boundary_and_safe_scope(self):
        today = datetime.now(timezone.utc).date()
        outside = self.work / "outside"
        outside.mkdir()
        sentinel = outside / "2000-01-01.log"
        sentinel.write_text("preserve outside diagnostic root")
        for service in ("algotrading.strategy", "algotrading.postgres"):
            directory = self.logs / service
            directory.mkdir()
            for age in range(9):
                (directory / f"{today - timedelta(days=age)}.log").write_text("diagnostic")
            (directory / "trades.csv").write_text("preserve trading evidence")
            (directory / "checkpoint.json").write_text("preserve checkpoint")
            (directory / "2000-02-30.log").write_text("invalid date, preserve")
        (self.logs / "algotrading.link").symlink_to(outside, target_is_directory=True)
        (self.logs / "algotrading.strategy/1999-01-01.log").symlink_to(sentinel)
        self.assertEqual(cleanup.clean(self.logs, today), 14)
        self.assertEqual(cleanup.clean(self.logs, today), 0)
        self.assertTrue(sentinel.exists())
        for service in ("algotrading.strategy", "algotrading.postgres"):
            directory = self.logs / service
            for age in range(2):
                self.assertTrue((directory / f"{today - timedelta(days=age)}.log").exists())
            self.assertTrue((directory / "trades.csv").exists())
            self.assertTrue((directory / "checkpoint.json").exists())
        # Quiet services must also expire when the UTC day advances.
        self.assertEqual(cleanup.clean(self.logs, today + timedelta(days=1)), 2)
        with self.assertRaises(ValueError):
            cleanup.clean(self.work, today)
        linked = self.work / "linked/algotrading"
        linked.parent.mkdir()
        linked.symlink_to(self.logs.parent, target_is_directory=True)
        with self.assertRaises(ValueError):
            cleanup.clean(linked / "services", today)

    def start_receiver(self, date_property="timegenerated", limit_bytes=cleanup.MAX_FILE_BYTES):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        state = self.work / "state"
        state.mkdir(exist_ok=True)
        config = (LOGGING / "rsyslog.conf").read_text().replace(
            "/var/lib/algotrading-logs", str(state)).replace(
            "/var/log/algotrading/services", str(self.logs)).replace('Port="5514"', f'Port="{port}"').replace(
            "/usr/local/lib/algotrading/clean_daily_logs.py", str(LOGGING / 'clean_daily_logs.py')).replace(
            '--max-bytes 52428800', f'--max-bytes {limit_bytes}')
        if date_property != "timegenerated":
            # Drive the real UTC date template across midnight without changing host time.
            config = config.replace('name="timegenerated" dateFormat=', f'name="{date_property}" dateFormat=')
        config_path = self.work / "rsyslog.conf"
        config_path.write_text(config)
        subprocess.run(["/usr/sbin/rsyslogd", "-N1", "-f", str(config_path)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        process = subprocess.Popen(["/usr/sbin/rsyslogd", "-n", "-i", str(self.work / "receiver.pid"),
                                    "-f", str(config_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                self.fail("Receiver exited before accepting a connection")
            try:
                connection = socket.create_connection(("127.0.0.1", port), timeout=.2)
                connection.close()
                break
            except OSError:
                time.sleep(.05)
        else:
            process.terminate()
            process.wait(timeout=5)
            self.fail("Receiver did not start")
        return process, port

    def send(self, port, tag, message, timestamp="2000-01-01T00:00:00.000000Z", severity=6):
        record = f"<{24 + severity}>1 {timestamp} localhost {tag} 123 - - {message}\n"
        with socket.create_connection(("127.0.0.1", port), timeout=2) as connection:
            connection.sendall(record.encode())

    def stop(self, process):
        process.terminate()
        process.wait(timeout=5)

    def wait_for(self, path, marker):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if path.exists() and marker in path.read_text():
                return path.read_text()
            time.sleep(.05)
        self.fail("Expected diagnostic marker was not persisted")

    def test_receiver_appends_across_restart_recreation_and_redacts(self):
        today = datetime.now(timezone.utc).date()
        path = self.logs / f"algotrading.market-data/{today}.log"
        process, port = self.start_receiver()
        try:
            self.send(port, "algotrading.market-data", "before-restart")
            self.wait_for(path, "before-restart")
            for message in ("password=PRIVATE_FIXTURE", '"api_key":"PRIVATE_FIXTURE"',
                            "Authorization: Bearer PRIVATE_FIXTURE", "postgres://user:PRIVATE_FIXTURE@host/db",
                            "https://api.telegram.org/bot123:PRIVATE_FIXTURE/sendMessage"):
                self.send(port, "algotrading.market-data", message)
            self.send(port, "algotrading.market-data", "after-redaction", severity=3)
            text = self.wait_for(path, "after-redaction")
            self.assertEqual(path.stat().st_mode & 0o777, 0o640)
            self.assertEqual(path.parent.stat().st_mode & 0o777, 0o750)
            self.assertNotIn("PRIVATE_FIXTURE", text)
            self.assertEqual(text.count("[REDACTED:"), 5)
            self.assertIn("severity=err", text)
            self.assertIn("source_time=2000-01-01", text)
            self.send(port, "../../unsafe", "must-not-persist")
        finally:
            self.stop(process)
        # New daemon/connection, same service name: represents a recreated container.
        process, port = self.start_receiver()
        try:
            self.send(port, "algotrading.market-data", "after-recreation")
            text = self.wait_for(path, "after-recreation")
            self.assertIn("before-restart", text)
        finally:
            self.stop(process)
        self.assertFalse(any("must-not-persist" in p.read_text() for p in self.logs.rglob("*.log")))

    def test_utc_daily_template_splits_at_midnight(self):
        process, port = self.start_receiver(date_property="timereported")
        try:
            self.send(port, "algotrading.strategy", "day-one", "2026-10-06T23:59:59Z")
            self.send(port, "algotrading.strategy", "day-two", "2026-10-07T01:00:00+01:00")
            first = self.wait_for(self.logs / "algotrading.strategy/2026-10-06.log", "day-one")
            second = self.wait_for(self.logs / "algotrading.strategy/2026-10-07.log", "day-two")
            self.assertNotIn("day-two", first)
            self.assertNotIn("day-one", second)
        finally:
            self.stop(process)

    def test_receiver_trims_and_continues_in_the_same_daily_file(self):
        today = datetime.now(timezone.utc).date()
        path = self.logs / f"algotrading.strategy/{today}.log"
        process, port = self.start_receiver(limit_bytes=4096)
        try:
            self.send(port, "algotrading.strategy", "oldest-marker")
            self.wait_for(path, "oldest-marker")
            inode = path.stat().st_ino
            for index in range(45):
                self.send(port, "algotrading.strategy", f"record-{index:03d}-" + "x" * 150)
                self.wait_for(path, f"record-{index:03d}-")
            self.send(port, "algotrading.strategy", "newest-marker")
            text = self.wait_for(path, "newest-marker")
            self.assertIn("LOG-TRIM", text)
            self.assertNotIn("oldest-marker", text)
            self.assertNotIn("\x00", text)
            self.assertLessEqual(path.stat().st_size, 4096)
            self.assertEqual(path.stat().st_ino, inode)
            self.assertEqual(list(path.parent.iterdir()), [path])
        finally:
            self.stop(process)
        process, port = self.start_receiver(limit_bytes=4096)
        try:
            self.send(port, "algotrading.strategy", "after-trim-restart")
            text = self.wait_for(path, "after-trim-restart")
            self.assertIn("newest-marker", text)
        finally:
            self.stop(process)

    def test_trim_preserves_recent_complete_lines_and_ignores_symlinks(self):
        directory = self.logs / "algotrading.strategy"
        directory.mkdir()
        path = directory / "2026-10-08.log"
        path.write_bytes(b"old line\n" * 500 + b"latest complete line\n")
        outside = self.work / "outside.log"
        outside.write_bytes(b"outside" * 1000)
        (directory / "2026-10-07.log").symlink_to(outside)
        checkpoint = directory / "checkpoint.json"
        checkpoint.write_bytes(b"preserve" * 1000)
        self.assertEqual(cleanup.trim(self.logs, 4096), 1)
        self.assertLessEqual(path.stat().st_size, 2048)
        self.assertTrue(path.read_bytes().endswith(b"latest complete line\n"))
        self.assertEqual(cleanup.trim(self.logs, 4096), 0)
        self.assertEqual(outside.stat().st_size, 7000)
        self.assertEqual(checkpoint.stat().st_size, 8000)
        with self.assertRaises(ValueError):
            cleanup.trim(self.work, 4096)

    def test_large_single_record_and_unsafe_writer_paths(self):
        header = b"2026-10-08T12:00:00Z service=algotrading.strategy "
        cleanup.append_record(self.logs, header + ("large diagnostic é" * 500).encode() + b"\n", 4096)
        path = self.logs / "algotrading.strategy/2026-10-08.log"
        self.assertIn("oversized diagnostic record omitted", path.read_text())
        self.assertLessEqual(path.stat().st_size, 4096)
        cleanup.append_record(self.logs, header + b"next normal record\n", 4096)
        self.assertTrue(path.read_bytes().endswith(b"next normal record\n"))
        with self.assertRaises(ValueError):
            cleanup.append_record(self.work, header + b"invalid root\n", 4096)
        with self.assertRaises(ValueError):
            cleanup.append_record(self.logs, b"2026-10-08 service=../../outside invalid service\n", 4096)
        outside = self.work / "outside"
        outside.mkdir()
        (self.logs / "algotrading.link").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            cleanup.append_record(self.logs, b"2026-10-08 service=algotrading.link must not escape\n", 4096)
        self.assertEqual(list(outside.iterdir()), [])

    def test_default_fifty_mib_cap_on_an_existing_oversized_file(self):
        directory = self.logs / "algotrading.strategy"
        directory.mkdir()
        path = directory / "2026-10-08.log"
        with path.open("wb") as handle:
            handle.write(b"old diagnostic line\n" * 3000000)
        self.assertGreater(path.stat().st_size, cleanup.MAX_FILE_BYTES)
        cleanup.append_record(self.logs, b"2026-10-08T12:00:00Z service=algotrading.strategy newest line\n")
        self.assertLessEqual(path.stat().st_size, cleanup.MAX_FILE_BYTES)
        self.assertTrue(path.read_bytes().endswith(b"newest line\n"))
        self.assertEqual(list(directory.iterdir()), [path])

    def test_compose_overrides_only_logging_and_stdout_settings(self):
        environment = os.environ.copy()
        environment.update(POSTGRES_PASSWORD="test-only-placeholder", DASHBOARD_DATA_PROVIDER="real",
                           DASHBOARD_VIEWER_PASSWORD="test-only-placeholder", DASHBOARD_OPERATOR_PASSWORD="test-only-placeholder",
                           DASHBOARD_DOMAIN="localhost")
        fake_env = self.work / "empty.env"
        fake_env.write_text("")
        for base, overlay, count in (("deploy/live/docker-compose.yml", "deploy/live/docker-compose.logging.yml", 8),
                                     ("dashboard/docker-compose.production.yml", "dashboard/docker-compose.logging.yml", 2)):
            command = ["docker", "compose", "--env-file", str(fake_env), "--profile", "*", "-f", str(ROOT / base)]
            original = json.loads(subprocess.check_output(command + ["config", "--format", "json"], env=environment))
            merged = json.loads(subprocess.check_output(command + ["-f", str(ROOT / overlay), "config", "--format", "json"], env=environment))
            self.assertEqual(len(merged["services"]), count)
            for name, service in merged["services"].items():
                options = service["logging"]["options"]
                self.assertEqual(service["logging"]["driver"], "syslog")
                self.assertEqual(options["syslog-address"], "tcp://127.0.0.1:5514")
                self.assertNotIn("max-size", options)
                self.assertNotIn("max-file", options)
                before = original["services"][name]
                service.pop("logging", None)
                before.pop("logging", None)
                for settings in (service.get("environment", {}), before.get("environment", {})):
                    settings.pop("ALGOTRADING_LOG_QUIET", None)
                    settings.pop("ALGOTRADING_LOG_DIR", None)
                self.assertEqual(service, before, name)


if __name__ == "__main__":
    unittest.main()
