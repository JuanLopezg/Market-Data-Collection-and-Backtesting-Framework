#!/usr/bin/env python3
"""Exercise current adapters against disposable NATS/PostgreSQL, without LIVE services."""
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, text=True, capture_output=True,
                          timeout=120, **kwargs)


def wait_ready(name, port, postgres=False):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                if not postgres or subprocess.run(
                    # The image's temporary initialization server listens only on
                    # Unix sockets. Require its final TCP server before testing.
                    ["docker", "exec", name, "pg_isready", "-h", "127.0.0.1", "-U", "validation"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    timeout=2).returncode == 0:
                    return
        except (OSError, subprocess.TimeoutExpired):
            pass
        time.sleep(0.1)
    raise RuntimeError("Isolated infrastructure did not become ready: " + name)


def mapped_port(name, container_port):
    address = run(["docker", "port", name, str(container_port)]).stdout.strip()
    return int(address.rsplit(":", 1)[1])


def main():
    if os.name != "posix":
        raise RuntimeError("Run this integration fixture under WSL/Linux")
    # Require cached images rather than fetching dependencies or changing LIVE images.
    for image in ("nats:2.10-alpine", "postgres:16-alpine"):
        run(["docker", "image", "inspect", image])
    identity = "algotrading-validation-" + uuid.uuid4().hex
    containers = []
    with tempfile.TemporaryDirectory(prefix="algotrading-integration-") as temporary:
        directory = Path(temporary)
        executable = directory / "transport_persistence_test"
        flags = run(["pkg-config", "--cflags", "--libs", "libnats", "libpq"]).stdout.split()
        sources = [ROOT / "validation/transport_persistence_integration_test.cpp",
                   ROOT / "lib/src/transport/jetstream_bus.cpp",
                   ROOT / "lib/src/persistence/postgres_state_store.cpp"]
        includes = [ROOT / "lib/src" / name for name in
                    ("transport", "persistence", "execution", "rebalance", "data_types")]
        run(["c++", "-std=c++20", "-O0", *["-I" + str(path) for path in includes],
             *map(str, sources), *flags, "-o", str(executable)])
        password = secrets.token_hex(16)
        environment_file = directory / "postgres.env"
        environment_file.write_text("POSTGRES_USER=validation\nPOSTGRES_DB=validation\n"
                                    "POSTGRES_PASSWORD=" + password + "\n")
        environment_file.chmod(0o600)
        try:
            nats = identity + "-nats"
            postgres = identity + "-postgres"
            # Record owned names before creation, so failures after creation still clean up.
            containers.append(nats)
            run(["docker", "run", "-d", "--pull=never", "--name", nats,
                 "--label", "algotrading.validation=" + identity,
                 "-p", "127.0.0.1::4222", "nats:2.10-alpine",
                 "-js", "--store_dir", "/data"])
            containers.append(postgres)
            run(["docker", "run", "-d", "--pull=never", "--name", postgres,
                 "--label", "algotrading.validation=" + identity,
                 "--env-file", str(environment_file), "-p", "127.0.0.1::5432",
                 "postgres:16-alpine"])
            nats_port = mapped_port(nats, 4222)
            postgres_port = mapped_port(postgres, 5432)
            wait_ready(nats, nats_port)
            wait_ready(postgres, postgres_port, postgres=True)
            environment = dict(os.environ,
                TEST_NATS_URL=f"nats://127.0.0.1:{nats_port}",
                TEST_POSTGRES_DSN=f"host=127.0.0.1 port={postgres_port} dbname=validation "
                                  f"user=validation password={password} connect_timeout=3")
            crashed = subprocess.run([str(executable), "seed"], env=environment,
                                     capture_output=True, text=True, timeout=30)
            if crashed.returncode != 73:
                raise RuntimeError("Commit-before-ACK crash fixture failed: " + crashed.stderr)
            print("TRANSPORT-PERSISTENCE: PASS: transaction rollback and commit-before-ACK crash")
            for phase in ("recover", "verify"):
                # Abrupt infrastructure failure, retaining only this fixture's state.
                for name in containers:
                    run(["docker", "kill", name])
                    run(["docker", "start", name])
                # Docker may assign different ephemeral host ports after restart.
                nats_port = mapped_port(nats, 4222)
                postgres_port = mapped_port(postgres, 5432)
                environment["TEST_NATS_URL"] = f"nats://127.0.0.1:{nats_port}"
                environment["TEST_POSTGRES_DSN"] = (
                    f"host=127.0.0.1 port={postgres_port} dbname=validation "
                    f"user=validation password={password} connect_timeout=3")
                wait_ready(nats, nats_port)
                wait_ready(postgres, postgres_port, postgres=True)
                completed = run([str(executable), phase], env=environment)
                print(completed.stdout.strip())
        finally:
            for name in reversed(containers):
                # Never remove anything outside the exact UUID-owned fixture names.
                inspected = subprocess.run(
                    ["docker", "inspect", "--format",
                     '{{index .Config.Labels "algotrading.validation"}}', name],
                    capture_output=True, text=True, timeout=10)
                if inspected.returncode == 0 and inspected.stdout.strip() == identity:
                    run(["docker", "rm", "-fv", name])
    print("TRANSPORT-PERSISTENCE: PASS: isolated infrastructure removed")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        # Do not print subprocess arguments or private test connection settings.
        print("TRANSPORT-PERSISTENCE: FAIL: subprocess returned", error.returncode)
        print(error.stderr)
        raise SystemExit(1)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print("TRANSPORT-PERSISTENCE: FAIL:", error)
        raise SystemExit(1)
