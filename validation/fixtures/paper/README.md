# Isolated PAPER validation fixture

`compose.yml` is the topology used by `validation/paper_trading_test.py`, its
opt-in recovery study and the shadow rehearsal. It retains its long service names
and named-volume contracts so those fault/recovery tests exercise the same state
boundaries. It is not a second supported operator deployment.

The test generates a random project, private credentials, a controlled Binance
HTTP fixture, internal trading network, loopback dashboard port and TEST_FILE
notification settings. Only its own containers/networks are stopped; evidence and
volumes remain in ignored `storage/paper_validation/`. Never use this topology for
the running PAPER or a private exchange. The maintained runtime is `deploy/chain/`.

Build current images under WSL with `deploy/chain/algo build`, then run:

```bash
python3 validation/paper_trading_test.py
PAPER_RECOVERY_TEST=1 python3 validation/paper_trading_test.py
python3 validation/shadow_trading_test.py
```

Optional `PAPER_RUNTIME_IMAGE`, `PAPER_API_IMAGE`, `PAPER_WEB_IMAGE` override
packaged images for isolated acceptance. Default images are the current chain
images. `logging.yml` is rendered to check the logging contract; its syslog
receiver is not used by the internal-network fixture.

Recovery quiesces writers and drains the simulated exchange outbox, takes a
logical PostgreSQL dump and archives stopped market/NATS/observer volumes, then
restores into fresh owned volumes. Credentials are separate. This bounded study
does not establish online backup, unattended reconnection or VPS acceptance.
See validation/README.md for coverage and limits.
