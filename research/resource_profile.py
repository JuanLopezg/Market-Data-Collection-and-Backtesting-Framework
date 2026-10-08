"""Own the isolated resource campaign lifecycle; canonical replay owns economics."""
import copy
from datetime import datetime, timezone
import http.cookiejar
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time
import urllib.request

import yaml

from resource_report import parse_sample, write_report


def windows_path(path):
    if len(path.parts) > 3 and path.parts[1] == "mnt" and len(path.parts[2]) == 1:
        return path.parts[2].upper() + ":\\" + "\\".join(path.parts[3:])
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def open_windows(sandbox, target):
    if not Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe").exists(): return
    launcher = sandbox.path / "open_profile.ps1"
    launcher.write_text("param([string]$Target)\nStart-Process -FilePath $Target\n")
    try:
        subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", windows_path(launcher),
                        "-Target", target], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        pass  # The printed URL/report remains available if the desktop cannot open it.


def campaign_module(root):
    spec = importlib.util.spec_from_file_location("local_campaign", root / "validation/local_service_campaign.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def install_dashboard(sandbox, campaign, replay_dir, runner, replay_command):
    """Derive current dashboard deployments, replacing shared state and venue settings."""
    root = campaign.ROOT
    for context in (root / "dashboard", root / "dashboard/dashboard-api"):
        exclusions = set((context / ".dockerignore").read_text().splitlines())
        if not {".env*", "*.key", "*.pem", "**/.env*", "**/*.key", "**/*.pem"}.issubset(exclusions):
            raise ValueError("Dashboard build must exclude private environment/key files")
    images = {}
    for name, context in (("api", root / "dashboard/dashboard-api"), ("web", root / "dashboard")):
        tag = "algotrading-profile-" + name + ":" + sandbox.project
        command = ["docker", "build", "--pull=false", "-t", tag]
        if name == "web":
            command += ["--build-arg", "VITE_DASHBOARD_DATA_MODE=api"]
        campaign.command([*command, str(context)], sandbox.path, timeout=900)
        images[name] = campaign.command(["docker", "image", "inspect", tag, "--format", "{{.Id}}"], sandbox.path).strip()
    topology = yaml.safe_load((sandbox.path / "compose.yml").read_text())
    base = yaml.safe_load((root / "dashboard/docker-compose.yml").read_text())
    real = yaml.safe_load((root / "dashboard/docker-compose.real.yml").read_text())
    simulation = replay_dir / "simulation"
    simulation.mkdir(mode=0o755, exist_ok=True)
    dsn = "host=postgres port=5432 dbname=algotrading_historical_replay user=algotrading password=${SANDBOX_PASSWORD}"
    stores = ("dashboard-watchdog-data", "dashboard-manual-audit-data", "dashboard-alert-ack-data", "dashboard-alert-notifier-data")
    for store in stores:
        topology["volumes"][store] = {"name": sandbox.project + "-" + store}
    topology["networks"]["profile-web"] = {"name": sandbox.project + "-web"}
    for name in ("dashboard-api", "dashboard-web", "dashboard-watchdog", "dashboard-alert-notifier"):
        service = copy.deepcopy(base["services"].get(name, real["services"].get(name)))
        service.pop("build", None)
        service.pop("container_name", None)
        service["image"] = images["web" if name == "dashboard-web" else "api"]
        service["restart"] = "no"
        service["networks"] = ["historical-replay"]
        service["logging"] = {"driver": "json-file", "options": {"max-size": "2m", "max-file": "3"}}
        if name == "dashboard-api":
            service["environment"].update({"DASHBOARD_DATA_PROVIDER": "simulation", "DASHBOARD_RUNTIME_MODE": "REPLAY",
                                           "DASHBOARD_SIMULATION_DIR": "/data/simulation"})
            service["volumes"] = [str(simulation) + ":/data/simulation:ro"]
        elif name == "dashboard-web":
            service["ports"] = ["127.0.0.1::80"]
            service["networks"].append("profile-web")
        elif name == "dashboard-watchdog":
            service["environment"] = {"DASHBOARD_POSTGRES_DSN": dsn, "DASHBOARD_NATS_URL": "nats://nats:4222",
                "DASHBOARD_NATS_MONITOR_URL": "http://nats:8222", "DASHBOARD_NATS_STREAM": "ALGOTRADING_HISTORICAL_RUNTIME",
                "DASHBOARD_RUNTIME_MODE": "REPLAY", "DASHBOARD_MARKET_DATA_DB": "/data/market/database.db",
                "DASHBOARD_ALERT_STORE_DIR": "/data/watchdog", "DASHBOARD_WATCHDOG_INTERVAL": "15s",
                "DASHBOARD_WATCHDOG_SWEEP_TIMEOUT": "8s", "DASHBOARD_EXECUTION_VENUE": "HYPERLIQUID",
                "DASHBOARD_VENUE_TARGET_ENVIRONMENT": "TESTNET", "DASHBOARD_EXCHANGE_GATEWAY_MODE": "hyperliquid-dry-run",
                "DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL": "https://api.hyperliquid-testnet.xyz/info",
                "DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT": "1s"}
            service["volumes"] = ["market-data-db:/data/market", "dashboard-watchdog-data:/data/watchdog"]
        else:
            service["environment"] = {"DASHBOARD_ALERT_STORE_DIR": "/data/watchdog",
                "DASHBOARD_NOTIFIER_TEST_SINK_FILE": "/data/notifier/test-sink.jsonl", "DASHBOARD_NOTIFIER_SINK": "TEST_FILE",
                "DASHBOARD_NOTIFIER_INTERVAL": "5s", "DASHBOARD_NOTIFIER_COOLDOWN": "5m"}
        topology["services"][name] = service
    # The runner mounts only its own writable output and immutable historical inputs.
    def container_path(value):
        path = Path(str(value))
        if path == runner: return "/profile/runner"
        if path == campaign.ROOT / "deploy/historical_replay/run/1d_cmc_by_date.csv": return "/input.csv"
        if path == campaign.ROOT / "config/historical_replay/step56a_source_symbol_map_v1.csv": return "/mapping.csv"
        if path.is_absolute() and path.is_relative_to(replay_dir): return "/work/" + str(path.relative_to(replay_dir))
        return str(value)
    topology["services"]["profile-replay"] = {"image": sandbox.manifest["image"], "restart": "no",
        "user": str(os.getuid()) + ":" + str(os.getgid()), "networks": ["historical-replay"],
        "entrypoint": ["/bin/sh"], "command": ["-c", "while [ ! -f /work/start ]; do sleep 0.1; done; exec /profile/runner \"$@\"",
            "profile", *[container_path(value) for value in replay_command[1:]]],
        "volumes": [str(runner) + ":/profile/runner:ro", str(replay_dir) + ":/work",
            str(campaign.ROOT / "deploy/historical_replay/run/1d_cmc_by_date.csv") + ":/input.csv:ro",
            str(campaign.ROOT / "config/historical_replay/step56a_source_symbol_map_v1.csv") + ":/mapping.csv:ro"],
        "read_only": True, "tmpfs": ["/tmp"], "cap_drop": ["ALL"], "security_opt": ["no-new-privileges:true"],
        "logging": {"driver": "json-file", "options": {"max-size": "2m", "max-file": "3"}}}
    compose_path = sandbox.path / "compose.yml"
    compose_path.write_text(yaml.safe_dump(topology, sort_keys=False))
    sandbox.manifest["files"]["compose.yml"] = campaign.digest(compose_path)
    (sandbox.path / "manifest.json").write_text(json.dumps(sandbox.manifest, indent=2) + "\n")
    return campaign.Sandbox(sandbox.path)


class Measurement:
    """Collect Docker samples and one authenticated read-only dashboard client."""
    def __init__(self, sandbox, seconds, url, campaign):
        self.sandbox = sandbox
        self.seconds = seconds
        self.url = url
        self.campaign = campaign
        self.started = time.monotonic()
        self.stop = threading.Event()
        self.processes = []
        self.threads = []
        self.errors = []
        self.rows = []
        self.latest = {}
        self.lock = threading.Lock()
        self.requests = {"successful": 0, "failed": 0}

    def stream(self, arguments, output_name, docker=False):
        process = subprocess.Popen(arguments, cwd=self.campaign.ROOT, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        self.processes.append(process)
        def collect():
            with (self.sandbox.path / output_name).open("w") as output:
                for line in process.stdout:
                    if self.stop.is_set(): break
                    try:
                        sample = parse_sample(line)
                    except ValueError:
                        if line.strip():
                            with (self.sandbox.path / (output_name + ".diagnostics.log")).open("a") as diagnostics:
                                diagnostics.write(self.campaign.redact(line))
                        continue
                    row = {"seconds": time.monotonic() - self.started, "sample": sample}
                    output.write(json.dumps(row) + "\n")
                    output.flush()
                    if docker:
                        with self.lock: self.latest[sample["Name"]] = row
                    else:
                        with self.lock: self.rows.append(row)
        thread = threading.Thread(target=collect, daemon=True)
        thread.start()
        self.threads.append(thread)

    def start(self):
        ids = self.sandbox.compose("ps", "-q").split()
        self.stream(["docker", "stats", "--format", "{{json .}}", *ids], "docker_samples.jsonl", docker=True)
        # Windows host figures include all applications. Browser and WSL memory
        # are overlapping subsets, never added to the container working-set total.
        if Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe").exists():
            script = self.sandbox.path / "host_sample.ps1"
            script.write_text("""$ErrorActionPreference = 'Stop'
while ($true) {
  $cpu = Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter \"Name='_Total'\"
  $memory = Get-CimInstance Win32_OperatingSystem
  $browser = (Get-Process chrome,msedge,firefox -ErrorAction SilentlyContinue | Measure-Object WorkingSet64 -Sum).Sum
  $wsl = (Get-Process vmmemWSL,vmmem -ErrorAction SilentlyContinue | Measure-Object WorkingSet64 -Sum).Sum
  @{cpu_percent=$cpu.PercentProcessorTime; ram_bytes=1024*($memory.TotalVisibleMemorySize-$memory.FreePhysicalMemory);
    browser_ram_bytes=$browser; wsl_ram_bytes=$wsl} | ConvertTo-Json -Compress
  Start-Sleep -Seconds 5
}
""")
            try:
                # Conversion of a newly written NTFS file can race WSL metadata
                # visibility. Drive-mounted paths need no existence-dependent call.
                path = windows_path(script)
                self.stream(["powershell.exe", "-NoProfile", "-NonInteractive", "-File", path], "host_samples.jsonl")
            except (OSError, subprocess.CalledProcessError):
                pass  # Optional host sampling is reported unavailable; Docker proof remains required.
        thread = threading.Thread(target=self.read_dashboard, daemon=True)
        thread.start()
        self.threads.append(thread)

    def read_dashboard(self):
        opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        request = urllib.request.Request(self.url + "/api/auth/login",
            data=json.dumps({"username": "viewer", "password": "viewer-demo"}).encode(),
            headers={"Content-Type": "application/json"})
        try:
            with opener.open(request, timeout=3) as response: response.read()
            while not self.stop.is_set():
                for route in ("overview", "positions", "pipeline", "risk", "infrastructure", "ledger"):
                    if self.stop.is_set(): break
                    try:
                        with opener.open(self.url + "/api/" + route, timeout=2) as response: response.read()
                        self.requests["successful"] += 1
                    except OSError:
                        self.requests["failed"] += 1
                self.stop.wait(5)
        except OSError:
            self.errors.append("Dashboard authentication/read client failed")

    def finish(self):
        self.stop.set()
        for process in self.processes:
            if process.poll() is None: process.terminate()
        for process in self.processes:
            try: process.wait(timeout=3)
            except subprocess.TimeoutExpired: process.kill()
        for thread in self.threads: thread.join(timeout=3)


def profile(args, replay):
    if os.name != "posix":
        # Argument vectors cross into WSL directly; no shell interpolation.
        script = subprocess.check_output(["wsl", "--exec", "wslpath", "-u", str(Path(__file__).with_name("replay.py"))], text=True).strip()
        subprocess.run(["wsl", "--exec", "python3", script, "resources", "--days", str(args.days), "--seconds", str(args.seconds),
                        *(["--skip-build"] if args.skip_build else []), *(["--open"] if args.open else [])], check=True)
        return {"result": "PASS", "report": "See the WSL report path above"}
    if not 2 <= args.days <= 107 or not 10 <= args.seconds <= 300:
        raise ValueError("Choose 2..107 days and 10..300 seconds for this bounded local profile")
    replay.ensure_required_files()
    campaign = campaign_module(replay.ROOT)
    campaign.require_local_docker()
    runner_source = replay.build_full_runner()
    print("RESOURCE-PROFILE: preparing isolated services and dashboard; setup is outside the timed window", flush=True)
    sandbox = campaign.prepare(args.skip_build)
    runner = sandbox.path / "canonical-runner"
    shutil.copyfile(runner_source, runner)
    runner.chmod(0o755)
    run_dir = replay.RUN_ROOT / (sandbox.project + "-resources")
    run_dir.mkdir(mode=0o755)
    measurement = None
    metadata = {"result": "FAIL", "days": args.days, "requested_seconds": args.seconds,
                "project": sandbox.project, "dashboard_provider": "simulation", "errors": [],
                "canonical_runner_sha256": campaign.digest(runner), "service_build_reused": args.skip_build}
    summary = {}
    try:
        sandbox.compose("up", "-d", timeout=180)
        sandbox.wait()
        replay_args = replay.parser().parse_args(["dashboard", "--days", str(args.days), "--label", run_dir.name,
            "--no-dashboard-up", "--ui-delay-ms", str(int(args.seconds * 250 / (args.days * 2)))])
        def execute_in_container(command, *, log):
            nonlocal sandbox, measurement
            sandbox = install_dashboard(sandbox, campaign, run_dir, runner, command)
            sandbox.compose("config", "--quiet")
            names = ("dashboard-api", "dashboard-web", "dashboard-watchdog", "dashboard-alert-notifier", "profile-replay")
            sandbox.compose("up", "-d", *names, timeout=180)
            address = sandbox.compose("port", "dashboard-web", "80").strip()
            url = "http://" + address
            ready_deadline = time.monotonic() + 45
            while time.monotonic() < ready_deadline:
                try:
                    with urllib.request.urlopen(url + "/api/health", timeout=3) as response:
                        if response.status == 200: break
                except OSError:
                    time.sleep(1)
            else:
                raise RuntimeError("Dashboard loopback URL did not become healthy before measurement")
            required = {*campaign.SERVICES, "historical-market-data", "nats", "postgres", *names}
            if not required.issubset(set(sandbox.compose("ps", "--status", "running", "--services").split())):
                raise RuntimeError("Every requested service must be running before measurement")
            print("RESOURCE-PROFILE: dashboard " + url + " (viewer / viewer-demo)", flush=True)
            if args.open: open_windows(sandbox, url)
            print("RESOURCE-PROFILE: measuring " + str(args.seconds) + " seconds; stack stops automatically", flush=True)
            measurement = Measurement(sandbox, args.seconds, url, campaign)
            measurement.start()
            metadata["dashboard_url"] = url
            metadata["started_utc"] = datetime.now(timezone.utc).isoformat()
            replay_id = sandbox.compose("ps", "-q", "profile-replay").strip()
            deadline = measurement.started + args.seconds
            (run_dir / "start").touch()
            completed = None
            samples = []
            while time.monotonic() < deadline:
                with measurement.lock:
                    samples.append({"seconds": time.monotonic() - measurement.started,
                                    "containers": copy.deepcopy(measurement.latest)})
                if (run_dir / "runtime_summary.json").exists() and completed is None:
                    completed = time.monotonic() - measurement.started
                measurement.stop.wait(min(1, max(0, deadline - time.monotonic())))
            metadata["measured_seconds"] = time.monotonic() - measurement.started
            metadata["replay_completed_seconds"] = completed
            measurement.finish()
            metadata["dashboard_requests"] = measurement.requests
            metadata["errors"].extend(measurement.errors)
            metadata["samples"] = samples
            metadata["host_samples"] = measurement.rows
            metadata["host_sampling_available"] = bool(measurement.rows)
            running_at_end = set(sandbox.compose("ps", "--status", "running", "--services").split())
            metadata["running_services_at_end"] = sorted(running_at_end)
            if not (required - {"profile-replay"}).issubset(running_at_end):
                raise RuntimeError("A background service exited during measurement")
            sandbox.compose("stop", timeout=60)
            logs = sandbox.compose("logs", "--no-color", "--no-log-prefix", "profile-replay")
            log.write_text(campaign.redact(logs))
            status = json.loads(campaign.command(["docker", "inspect", replay_id], sandbox.path))[0]["State"]
            if completed is None or status["ExitCode"] != 0:
                raise RuntimeError("Backtest did not complete successfully inside the timed window; report retains partial measurements")
            complete_samples = sum(len(row["containers"]) == 14 and all(
                row["seconds"] - value["seconds"] <= 5 for value in row["containers"].values()) for row in samples)
            metadata["complete_container_samples"] = complete_samples
            if not samples or complete_samples < 0.7 * len(samples):
                raise RuntimeError("Resource coverage is incomplete: expected trading, infrastructure, dashboard and replay containers")
            if not measurement.requests["successful"] or measurement.errors:
                raise RuntimeError("Authenticated dashboard workload was not observed")
        old_noninteractive = os.environ.get("REALTEST_NONINTERACTIVE")
        os.environ["REALTEST_NONINTERACTIVE"] = "1"
        try:
            summary = replay.execute_full(replay_args, run_command=execute_in_container, state_dir=run_dir / "simulation")
        finally:
            if old_noninteractive is None: os.environ.pop("REALTEST_NONINTERACTIVE", None)
            else: os.environ["REALTEST_NONINTERACTIVE"] = old_noninteractive
        metadata["result"] = "PASS"
        metadata["replay"] = summary
    except (Exception, SystemExit, KeyboardInterrupt) as error:
        metadata["errors"].append(campaign.redact(str(error)).strip())
    finally:
        if measurement: measurement.finish()
        # Shutdown is scoped by Sandbox's validated project/network/volume guard.
        try:
            sandbox.compose("stop", timeout=60)
            (sandbox.path / "profile_services.log").write_text(campaign.redact(sandbox.compose("logs", "--no-color")))
            if sandbox.compose("ps", "--status", "running", "--services").strip():
                raise RuntimeError("Generated project still has running containers")
        except Exception:
            metadata["result"] = "FAIL"
            metadata["errors"].append("Sandbox stop failed; inspect its own project")
        public = {key: value for key, value in metadata.items() if key not in ("samples", "host_samples")}
        (sandbox.path / "profile_summary.json").write_text(json.dumps(public, indent=2) + "\n")
        write_report(sandbox.path, metadata)
        if args.open: open_windows(sandbox, windows_path(sandbox.path / "resources.html"))
        print("RESOURCE-PROFILE: " + metadata["result"] + "; report " + str(sandbox.path / "resources.html"), flush=True)
    return {"result": metadata["result"], "report": str(sandbox.path / "resources.html")}
