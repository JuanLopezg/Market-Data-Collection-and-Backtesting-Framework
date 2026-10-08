#!/usr/bin/env python3
"""Prepare and operate an isolated, bounded current-service recovery sandbox."""
import argparse
import copy
import csv
from datetime import date, datetime, timedelta, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import time
import uuid

import yaml

ROOT = Path(__file__).resolve().parents[1]
RUNS = ROOT / "storage/local_service_campaign"
SERVICES = ("strategy", "portfolio-risk", "execution-state", "order-planner",
            "exchange-gateway", "simulated-exchange")
CHECKPOINTS = ("strategy_market_update_checkpoint", "portfolio_risk_live_decision_checkpoint",
               "order_planner_live_notional_checkpoint")
SECRET = re.compile(r"(?i)(password|passwd|secret|token|api[_-]?key|authorization)[\" ']*[:=]|"
                    r"[a-z]+://[^ /]+:[^ /]+@|bot\d+:[a-z0-9_-]+|-----BEGIN")


def redact(text):
    return "\n".join("[REDACTED: credential-bearing diagnostic]" if SECRET.search(line)
                     else line for line in text.splitlines()) + "\n"


def command(arguments, directory=None, timeout=120):
    environment = dict(os.environ)
    environment.pop("SANDBOX_PASSWORD", None)
    result = subprocess.run(arguments, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=timeout)
    if directory:
        with (directory / "operations.log").open("a") as output:
            output.write(redact(result.stdout + result.stderr))
    if result.returncode:
        raise RuntimeError("Command failed; inspect the redacted operations log")
    return result.stdout


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_local_docker():
    endpoint = os.environ.get("DOCKER_HOST")
    if not endpoint:
        context = json.loads(command(["docker", "context", "inspect"]))
        endpoint = context[0]["Endpoints"]["docker"]["Host"]
    if not endpoint.startswith(("unix://", "npipe://")):
        raise ValueError("Select a local Docker context; remote Docker endpoints are excluded")


class Sandbox:
    def __init__(self, path):
        self.path = Path(path).absolute()
        if RUNS.resolve() != RUNS.absolute() or self.path.resolve() != self.path or RUNS.resolve() not in self.path.parents:
            raise ValueError("Sandbox must be inside storage/local_service_campaign")
        self.manifest = json.loads((self.path / "manifest.json").read_text())
        self.project = self.manifest["project"]
        if self.manifest["target"] != 20200419:
            raise ValueError("Unexpected synthetic fixture window")
        if not re.fullmatch(r"algotrading-local-[a-f0-9]{12}", self.project):
            raise ValueError("Invalid sandbox project identity")
        for name, expected in self.manifest["files"].items():
            if name not in ("compose.yml", "fixture.csv", "time.env", ".env") or digest(self.path / name) != expected:
                raise ValueError("Sandbox configuration changed; prepare a fresh run")
        topology = yaml.safe_load((self.path / "compose.yml").read_text())
        network = topology["networks"]["historical-replay"]
        if topology["name"] != self.project or not network["internal"] or network["name"] != self.project + "-net":
            raise ValueError("Sandbox network is not isolated")
        for entry in topology["volumes"].values():
            if not entry["name"].startswith(self.project + "-"):
                raise ValueError("Sandbox volume is outside its project")
        for service in topology["services"].values():
            networks = service.get("networks")
            if networks == ["historical-replay", "profile-web"]:
                web = topology["networks"].get("profile-web", {})
                if service is not topology["services"].get("dashboard-web") or web.get("name") != self.project + "-web":
                    raise ValueError("Only the sandbox web frontend may use its own loopback publishing network")
                if any(not port.startswith("127.0.0.1:") for port in service.get("ports", [])):
                    raise ValueError("Dashboard must publish on loopback only")
            elif networks != ["historical-replay"]:
                raise ValueError("Every sandbox container must use its own internal network")
            if "container_name" in service:
                raise ValueError("Every sandbox container must use its own internal network and generated name")
        self.dc = ["docker", "compose", "--project-name", self.project,
                   "--env-file", str(self.path / ".env"), "-f", str(self.path / "compose.yml")]

    def compose(self, *arguments, timeout=120):
        return command([*self.dc, *arguments], self.path, timeout)

    def sql(self, statement):
        return self.compose("exec", "-T", "postgres", "psql", "-U", "algotrading", "-d",
                            "algotrading_historical_replay", "-Atq", "-c", statement).strip()

    def publish(self, subject, payload):
        # Internal Docker networking deliberately excludes host TCP publication.
        # Build a disposable SDK client and run it inside this project's network.
        executable = self.path / "publisher"
        if not executable.exists():
            source = self.path / "publisher.cpp"
            source.write_text('''#include <nats/nats.h>
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    natsConnection* connection = nullptr;
    natsStatus status = natsConnection_ConnectTo(&connection, "nats://nats:4222");
    if (status == NATS_OK) status = natsConnection_PublishString(connection, argv[1], argv[2]);
    if (status == NATS_OK) status = natsConnection_Flush(connection);
    if (connection) natsConnection_Destroy(connection);
    return static_cast<int>(status);
}
''')
            flags = command(["pkg-config", "--cflags", "--libs", "libnats"]).split()
            command(["c++", str(source), *flags, "-o", str(executable)], self.path)
            executable.chmod(0o755)
        command(["docker", "run", "--rm", "--network", self.project + "-net",
                 "--label", "com.docker.compose.project=" + self.project,
                 "--label", "com.docker.compose.service=publisher",
                 "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--read-only",
                 "-v", str(executable) + ":/local-publisher:ro", "--entrypoint", "/local-publisher",
                 self.manifest["image"], subject, payload], self.path)

    def sample(self, phase):
        ids = self.compose("ps", "-q").split()
        if not ids:
            return
        rows = command(["docker", "stats", "--no-stream", "--format", "{{json .}}", *ids], self.path)
        with (self.path / "resources.jsonl").open("a") as output:
            for row in rows.splitlines():
                output.write(json.dumps({"utc": datetime.now(timezone.utc).isoformat(),
                                         "phase": phase, "sample": json.loads(row)}) + "\n")

    def capture(self):
        root = self.path / "algotrading/services"
        cursor_path = self.path / "log_cursors.json"
        cursors = json.loads(cursor_path.read_text()) if cursor_path.exists() else {}
        for service in (*SERVICES, "nats", "postgres", "historical-market-data"):
            container = self.compose("ps", "-a", "-q", service).strip()
            logs = self.compose("logs", "--no-color", "--no-log-prefix", "--timestamps", service)
            directory = root / ("algotrading." + self.project + "." + service)
            directory.mkdir(parents=True, exist_ok=True, mode=0o750)
            days = {}
            lines = redact(logs).splitlines()
            signatures = [hashlib.sha256(line.encode()).hexdigest() for line in lines]
            previous = cursors.get(service, {})
            offset = 0
            if previous.get("container") == container and previous.get("last") in signatures:
                offset = max(index + 1 for index, signature in enumerate(signatures) if signature == previous["last"])
            for line in lines[offset:]:
                day = line[:10] if re.match(r"\d{4}-\d{2}-\d{2}T", line) else datetime.now(timezone.utc).date().isoformat()
                days.setdefault(day, []).append(line + " container=" + container)
            for day, lines in days.items():
                path = directory / (day + ".log")
                with path.open("a") as output:
                    output.write("\n".join(lines) + "\n")
                path.chmod(0o640)
            if signatures:
                cursors[service] = {"container": container, "last": signatures[-1]}
        cursor_path.write_text(json.dumps(cursors, indent=2) + "\n")
        spec = importlib.util.spec_from_file_location("daily_cleanup", ROOT / "deploy/live/logging/clean_daily_logs.py")
        cleanup = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cleanup)
        cleanup.clean(root, datetime.now(timezone.utc).date())

    def evidence(self):
        result = {}
        for table in CHECKPOINTS:
            rows = self.sql(f"SELECT coalesce(json_agg(r ORDER BY timestamp),'[]'::json) FROM {table} r;")
            result[table] = json.loads(rows)
        result["account"] = json.loads(self.sql("SELECT snapshot::text FROM trading_runtime_state WHERE singleton=TRUE;"))
        result["fills"] = json.loads(self.sql("SELECT coalesce(json_agg(r ORDER BY fill_id),'[]'::json) FROM trading_fills r;"))
        result["backend"] = json.loads(self.sql("SELECT row_to_json(r) FROM simulated_exchange_state r;"))
        result["backend_positions"] = json.loads(self.sql("SELECT coalesce(json_object_agg(coin,quantity),'{}'::json) FROM simulated_exchange_positions;"))
        result["pending_outbox"] = int(self.sql("SELECT count(*) FROM simulated_exchange_outbox WHERE NOT published;"))
        return result

    def check(self):
        active = set(self.compose("ps", "--status", "running", "--services").split())
        if not set((*SERVICES, "nats", "postgres")).issubset(active):
            raise RuntimeError("Required sandbox services are not all running")
        result = self.evidence()
        target = self.manifest["target"]
        for table in CHECKPOINTS:
            rows = result[table]
            if len(rows) != 1 or rows[0]["timestamp"] != target:
                raise RuntimeError("Expected exactly one aligned checkpoint in " + table)
        strategy = result[CHECKPOINTS[0]][0]
        risk = result[CHECKPOINTS[1]][0]
        planner = result[CHECKPOINTS[2]][0]
        update = json.loads(strategy["update_payload"])
        intent = json.loads(strategy["intent_payload"])
        decision = json.loads(risk["decision_payload"])
        account = json.loads(risk["account_payload"])
        request = json.loads(planner["request_payload"])
        plan = json.loads(planner["plan_payload"])
        if (update["completed_through"] != target or intent["timestamp"] != target or
                decision["decision_timestamp"] != target or account["timestamp"] != target or
                request["decision_timestamp"] != target or plan["decision_timestamp"] != target or
                request["reference_closes"] != plan["reference_closes"]):
            raise RuntimeError("Causal timestamps/reference closes changed across the pipeline")
        execution_day = int((datetime.strptime(str(target), "%Y%m%d").date() + timedelta(days=1)).strftime("%Y%m%d"))
        if not result["fills"] or any(fill["timestamp"] != execution_day for fill in result["fills"]):
            raise RuntimeError("Fixture must execute real simulated fills at the following open")
        if ({fill["order_id"] for fill in result["fills"]} != {order["order_id"] for order in plan["submit_orders"]} or
                len(result["fills"]) != len(plan["submit_orders"])):
            raise RuntimeError("Not all fixture orders have completed their simulated fills")
        if sorted(result["account"]["processed_fill_ids"]) != sorted(fill["fill_id"] for fill in result["fills"]):
            raise RuntimeError("Processed fill IDs do not reconcile with the fill audit")
        if result["pending_outbox"] or result["account"]["account_cash"] != result["backend"]["cash"]:
            raise RuntimeError("Unpublished backend evidence or cash reconciliation drift")
        if result["account"]["account_positions"] != result["backend_positions"]:
            raise RuntimeError("Execution/backend position reconciliation drift")
        baseline_path = self.path / "baseline.json"
        if baseline_path.exists():
            baseline = json.loads(baseline_path.read_text())
            # Snapshot order maps and fill-ID sets can iterate differently after
            # reconstruction. Compare every record, preserving duplicate entries.
            for evidence in (baseline, result):
                evidence["account"]["processed_fill_ids"].sort()
                evidence["account"]["orders"].sort(key=lambda order: order["order_id"])
            # Backend snapshot request timestamps may advance; compare its economic fields.
            for key in (*CHECKPOINTS, "account", "fills"):
                if baseline[key] != result[key]:
                    raise RuntimeError("Durable evidence changed after recovery: " + key)
            if baseline["backend"]["next_fill_id"] != result["backend"]["next_fill_id"]:
                raise RuntimeError("Backend allocated new fill IDs after recovery")
        else:
            baseline_path.write_text(json.dumps(result, indent=2) + "\n")
        self.capture()
        self.sample("accepted")
        print("LOCAL-SERVICE: PASS: aligned checkpoints, fills, cash and recovery baseline", flush=True)
        return result

    def checkpoint_crash(self, service):
        if (self.path / "baseline.json").exists():
            raise ValueError("Checkpoint barrier requires a fresh prepared sandbox")
        table = dict(zip(("strategy", "portfolio-risk", "order-planner"), CHECKPOINTS))[service]
        self.compose("up", "-d", *SERVICES, timeout=180)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                if all(self.sql(f"SELECT count(*) FROM {name};") == "0" for name in CHECKPOINTS):
                    break
                raise ValueError("Checkpoint barrier requires empty service checkpoint stores")
            except RuntimeError:
                time.sleep(1)
        else:
            raise RuntimeError("Service stores did not initialize before checkpoint barrier")
        target = self.manifest["target"]
        self.sql("CREATE FUNCTION local_campaign_barrier() RETURNS trigger LANGUAGE plpgsql AS $$ "
                 f"BEGIN IF NEW.timestamp={target} THEN PERFORM pg_sleep(60); END IF; RETURN NEW; END $$; "
                 f"CREATE TRIGGER local_campaign_hold BEFORE INSERT ON {table} "
                 "FOR EACH ROW EXECUTE FUNCTION local_campaign_barrier();")
        self.compose("up", "-d", "historical-market-data")
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            pid = self.sql("SELECT pid FROM pg_stat_activity WHERE wait_event='PgSleep' "
                           "AND datname=current_database() AND query LIKE 'INSERT INTO " + table + "%';")
            if pid.isdigit():
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("Did not observe the publish-before-checkpoint SQL barrier")
        if self.sql(f"SELECT count(*) FROM {table} WHERE timestamp={target};") != "0":
            raise RuntimeError("Blocked checkpoint was already durable")
        self.compose("kill", service)
        # Stop only the observed blocked backend so disconnect cannot commit its INSERT.
        self.sql("SELECT pg_terminate_backend(" + pid + ");")
        self.sql(f"DROP TRIGGER local_campaign_hold ON {table}; DROP FUNCTION local_campaign_barrier();")
        if self.sql(f"SELECT count(*) FROM {table} WHERE timestamp={target};") != "0":
            raise RuntimeError("Crash barrier unexpectedly committed its checkpoint")
        (self.path / "checkpoint_crash.json").write_text(json.dumps(
            {"service": service, "table": table, "target": target,
             "observed_sql_barrier": True, "checkpoint_absent_after_crash": True}, indent=2) + "\n")
        self.compose("start", service)
        self.wait()
        print("LOCAL-SERVICE: PASS: checkpoint crash/redelivery boundary for " + service, flush=True)

    def outbox_crash(self):
        self.check()
        count = int(self.sql("SELECT count(*) FROM simulated_exchange_outbox;"))
        if not count:
            raise RuntimeError("Outbox crash fixture requires actual simulated order/fill events")
        # Model a lost publication receipt without modifying account/order/fill truth.
        self.sql("CREATE FUNCTION local_outbox_barrier() RETURNS trigger LANGUAGE plpgsql AS $$ "
                 "BEGIN IF NEW.published THEN PERFORM pg_sleep(60); END IF; RETURN NEW; END $$; "
                 "CREATE TRIGGER local_outbox_hold BEFORE UPDATE OF published ON simulated_exchange_outbox "
                 "FOR EACH ROW EXECUTE FUNCTION local_outbox_barrier(); "
                 "UPDATE simulated_exchange_outbox SET published=FALSE;")
        def request():
            self.publish("gateway.backend.snapshot.request.v1", json.dumps({"metadata": {
                "schema_version": 1, "message_id": "local-outbox-probe:" + uuid.uuid4().hex,
                "correlation_id": "local-outbox-probe", "produced_at": self.manifest["target"]}}))
        request()
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            pid = self.sql("SELECT pid FROM pg_stat_activity WHERE wait_event='PgSleep' "
                           "AND datname=current_database() AND query LIKE 'UPDATE simulated_exchange_outbox%';")
            if pid.isdigit(): break
            time.sleep(0.2)
        else:
            raise RuntimeError("Outbox publication receipt barrier was not observed")
        self.compose("kill", "simulated-exchange")
        self.sql("SELECT pg_terminate_backend(" + pid + ");")
        self.sql("DROP TRIGGER local_outbox_hold ON simulated_exchange_outbox; DROP FUNCTION local_outbox_barrier();")
        if int(self.sql("SELECT count(*) FROM simulated_exchange_outbox WHERE NOT published;")) == 0:
            raise RuntimeError("No pending publication receipts survived the injected crash")
        self.compose("start", "simulated-exchange")
        request()
        self.wait()
        (self.path / "outbox_crash.json").write_text(json.dumps(
            {"receipt_loss_fixture": True, "observed_publish_before_receipt_barrier": True,
             "recovered_records": count}, indent=2) + "\n")
        print("LOCAL-SERVICE: PASS: outbox receipt crash recovered without economic duplication", flush=True)

    def wait(self):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                return self.check()
            except (RuntimeError, ValueError, KeyError):
                time.sleep(1)
        self.capture()
        raise RuntimeError("Sandbox did not reach accepted state; inspect retained evidence")

    def resource_report(self):
        path = self.path / "resources.jsonl"
        if not path.exists(): raise ValueError("No resource samples yet; run monitor first")
        by_service = {}
        units = {"B": 1, "kB": 1000, "MB": 1000000, "GB": 1000000000,
                 "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
        for line in path.read_text().splitlines():
            row = json.loads(line)["sample"]
            memory = re.fullmatch(r"([0-9.]+)([a-zA-Z]+)", row["MemUsage"].split(" / ")[0])
            if not memory or memory[2] not in units:
                raise ValueError("Unknown Docker memory unit")
            by_service.setdefault(row["Name"], []).append(
                (float(row["CPUPerc"].rstrip("%")), float(memory[1]) * units[memory[2]]))
        report = {name: {"samples": len(rows),
                        "cpu_mean_percent": sum(row[0] for row in rows) / len(rows),
                        "cpu_sampled_peak_percent": max(row[0] for row in rows),
                        "ram_mean_bytes": sum(row[1] for row in rows) / len(rows),
                        "ram_sampled_peak_bytes": max(row[1] for row in rows)}
                  for name, rows in by_service.items()}
        (self.path / "resource_summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print("LOCAL-SERVICE: resource summary saved (sampled peaks, not complete-backtest capacity acceptance)")


def prepare(skip_build=False):
    if RUNS.resolve() != RUNS.absolute():
        raise ValueError("Sandbox storage path must not contain symlinks")
    for path in (ROOT / "config").rglob("*"):
        if path.is_symlink() or (path.is_file() and not path.name.endswith(".example") and
                                (path.name.startswith(".env") or path.suffix in (".env", ".pem", ".key"))):
            raise ValueError("Private configuration or symlink must not enter the runtime image")
    project = "algotrading-local-" + uuid.uuid4().hex[:12]
    directory = RUNS / project
    directory.mkdir(parents=True, mode=0o700)
    stages = [
        ("build", ["meson", "compile", "-C", os.environ.get("BUILD_DIR", "build"), "-j", "3"]),
        ("package", ["bash", "deploy/historical_replay/build_runtime_bundle.sh"]),
        ("image", ["docker", "build", "--pull=false", "-t", "algotrading-runtime:" + project,
                   "deploy/historical_replay/.runtime_bundle"])]
    for stage, arguments in stages:
        if stage == "build" and skip_build:
            continue
        print("LOCAL-SERVICE: preparing " + stage, flush=True)
        command(arguments, directory, timeout=900)
    topology = copy.deepcopy(yaml.safe_load((ROOT / "deploy/historical_replay/docker-compose.yml").read_text()))
    topology.pop("x-runtime")
    topology.pop("x-postgres-connection")
    topology["name"] = project
    image = command(["docker", "image", "inspect", "algotrading-runtime:" + project,
                     "--format", "{{.Id}}"], directory).strip()
    time_file = directory / "time.env"
    reference = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    time_file.write_text("ALGOTRADING_TIME_SPEED=1\nALGOTRADING_TIME_REAL_REFERENCE_UTC=" + reference +
                         "\nALGOTRADING_TIME_SIMULATED_REFERENCE_UTC=2020-04-20T12:00:00Z\n")
    with (directory / "fixture.csv").open("w", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("date", "symbol", "open", "high", "low", "close", "volume"))
        for index in range(111):
            for symbol, factor in (("BTC", 1), ("ETH", 0.1)):
                price = (100 + index * 2) * factor
                writer.writerow(((date(2020, 1, 1) + timedelta(days=index)).isoformat(),
                                 symbol, price, price + factor, price - factor, price + factor / 2, 100000))
    private_env = directory / ".env"
    private_env.write_text("SANDBOX_PASSWORD=" + secrets.token_hex(24) + "\n")
    private_env.chmod(0o600)

    def resolve(value):
        if isinstance(value, dict): return {key: resolve(item) for key, item in value.items()}
        if isinstance(value, list): return [resolve(item) for item in value]
        if not isinstance(value, str): return value
        def substitution(match):
            name, default = match.group(1), match.group(2)
            return "${SANDBOX_PASSWORD}" if name == "POSTGRES_PASSWORD" else (default or "")
        return re.sub(r"\$\{([A-Z0-9_]+)(?::[-?]([^}]*))?\}", substitution, value)

    topology = resolve(topology)
    for name, service in topology["services"].items():
        service["networks"] = ["historical-replay"]
        service.pop("container_name", None)
        service.pop("profiles", None)
        service.pop("build", None)
        service["restart"] = "no"
        service["logging"] = {"driver": "json-file", "options": {"max-size": "2m", "max-file": "3"}}
        if "env_file" in service:
            service["env_file"] = [str(time_file)]
            service["image"] = image
            service["environment"] = {"ALGOTRADING_LOG_DEBUG": "1", "ALGOTRADING_LOG_QUIET": "0", "ALGOTRADING_LOG_DIR": ""}
        if name == "historical-market-data":
            service["volumes"][0] = str(directory / "fixture.csv") + ":/data/historical/1d_cmc.csv:ro"
        if "ports" in service:
            service["ports"] = ["127.0.0.1::" + port.rsplit(":", 1)[1] for port in service["ports"]]
        if name == "postgres":
            service["healthcheck"]["test"] = ["CMD", "pg_isready", "-h", "127.0.0.1",
                                                   "-U", "algotrading", "-d", "algotrading_historical_replay"]
        if name == "nats":
            service["healthcheck"] = {"test": ["CMD", "wget", "-q", "-O", "-", "http://127.0.0.1:8222/healthz"],
                                      "interval": "1s", "timeout": "2s", "retries": 30}
        if "nats" in service.get("depends_on", {}):
            service["depends_on"]["nats"]["condition"] = "service_healthy"
    topology["networks"]["historical-replay"] = {"name": project + "-net", "internal": True}
    for name, volume in topology["volumes"].items():
        volume["name"] = project + "-" + name
    (directory / "compose.yml").write_text(yaml.safe_dump(topology, sort_keys=False))
    manifest = {"project": project, "image": image, "target": 20200419,
                "build_reused": skip_build,
                "prepared_utc": reference, "files": {name: digest(directory / name) for name in
                ("compose.yml", "fixture.csv", "time.env", ".env")}}
    (directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    sandbox = Sandbox(directory)
    sandbox.compose("config", "--quiet")
    (RUNS / "latest.txt").write_text(str(directory) + "\n")
    print("LOCAL-SERVICE: prepared " + str(directory.relative_to(ROOT)), flush=True)
    return sandbox


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "start", "stop", "check", "duplicate", "outbox-crash", "fault", "capture", "monitor", "report", "cleanup"))
    parser.add_argument("--run", type=Path, help="Prepared directory; defaults to the last prepared sandbox")
    parser.add_argument("--service", choices=(*SERVICES, "nats", "postgres", "historical-market-data"))
    parser.add_argument("--operation", choices=("kill", "start", "restart", "recreate"))
    parser.add_argument("--hold-service", choices=("strategy", "portfolio-risk", "order-planner"),
                        help="Observe and crash at a publish-before-checkpoint barrier on a fresh start")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--skip-build", action="store_true",
                        help="Prepare only: reuse already validated current binaries; packaging still checks artifacts")
    arguments = parser.parse_args()
    if os.name != "posix": raise RuntimeError("Run under WSL/Linux")
    require_local_docker()
    if arguments.action == "prepare":
        prepare(arguments.skip_build)
        return
    path = arguments.run or Path((RUNS / "latest.txt").read_text().strip())
    sandbox = Sandbox(path)
    if arguments.action == "start":
        if arguments.hold_service:
            sandbox.checkpoint_crash(arguments.hold_service)
        else:
            sandbox.compose("up", "-d", timeout=180)
            sandbox.wait()
    elif arguments.action == "check": sandbox.wait()
    elif arguments.action == "capture": sandbox.capture()
    elif arguments.action == "stop":
        sandbox.capture()
        sandbox.compose("stop")
        print("LOCAL-SERVICE: stopped sandbox; configuration, volumes and evidence retained")
    elif arguments.action == "report": sandbox.resource_report()
    elif arguments.action == "duplicate":
        baseline = sandbox.check()
        messages = [("market.data.updated.v1", baseline[CHECKPOINTS[0]][0]["update_payload"]),
                    ("strategy.intents.v1", baseline[CHECKPOINTS[0]][0]["intent_payload"]),
                    ("decision.batch.v1", baseline[CHECKPOINTS[1]][0]["decision_payload"]),
                    ("execution.plan.notional.request.v1", baseline[CHECKPOINTS[2]][0]["request_payload"]),
                    ("execution.plan.notional.v1", baseline[CHECKPOINTS[2]][0]["plan_payload"])]
        fills = json.loads(sandbox.sql("SELECT coalesce(json_agg(payload),'[]'::json) "
                                      "FROM simulated_exchange_outbox WHERE subject='gateway.backend.event.fill.v1';"))
        messages += [("execution.event.fill.v1", payload) for payload in fills]
        for subject, payload in messages:
            sandbox.publish(subject, payload)
            sandbox.publish(subject, payload)
        time.sleep(5)
        sandbox.check()
    elif arguments.action == "outbox-crash": sandbox.outbox_crash()
    elif arguments.action == "fault":
        if not arguments.service or not arguments.operation:
            parser.error("fault requires --service and --operation")
        if arguments.operation == "recreate":
            sandbox.compose("up", "-d", "--no-deps", "--force-recreate", arguments.service)
        else:
            sandbox.compose(arguments.operation, arguments.service)
        print("LOCAL-SERVICE: injected " + arguments.operation + " into " + arguments.service)
    elif arguments.action == "monitor":
        if not 1 <= arguments.seconds <= 3600: parser.error("seconds must be 1..3600")
        deadline = time.monotonic() + arguments.seconds
        next_capture = time.monotonic()
        while time.monotonic() < deadline:
            sandbox.sample("monitor")
            if time.monotonic() >= next_capture:
                sandbox.capture()
                next_capture = time.monotonic() + 60
            time.sleep(1)
    elif arguments.action == "cleanup":
        sandbox.capture()
        sandbox.compose("down", "--volumes", "--remove-orphans")
        print("LOCAL-SERVICE: removed only prepared project containers/network/volumes; evidence retained")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        print("LOCAL-SERVICE: FAIL: " + redact(str(error)).strip(), flush=True)
        raise SystemExit(1)
