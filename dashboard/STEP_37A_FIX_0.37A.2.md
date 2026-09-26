# Step 37A.2 — Critical-alert dependency-cycle fix

Observed after a clean Docker rebuild: Step 37A called Step 36, which recursively reached the Step 32 safety gate. The new SymbolRegistry read model could transiently fail one of its own read-only dependencies during startup, producing a generic CRITICAL `SYMBOL_REGISTRY_BLOCKED`. That CRITICAL then blocked Step 32, so Step 36/37A could not proceed far enough to validate the registry itself.

Fix:

- Registry state remains fail-closed: dependency read failure still returns `Status=BLOCKED`, `Validated=false`, `OrderRouting=DISABLED`.
- While order routing is disabled, only these external read-only dependency failures are surfaced as WARN instead of CRITICAL:
  - canonical market-data ranking unavailable
  - current strategy universe unavailable
  - current Hyperliquid TESTNET metadata unavailable
- Artifact decode/load failures remain CRITICAL.
- Step35-manifest/registry divergences remain CRITICAL.
- New/unregistered strategy or market-data symbols remain CRITICAL.
- Venue-absent / explicitly unsupported coverage remains WARN and non-routable.
- Step 37A's own runtime gate still requires `/api/symbol-registry` to become fully validated; this fix cannot turn a blocked registry into PASS.

This removes the circular precondition without weakening order-routing safety.
Script hardening:

- Step 37A now waits (bounded: 15 attempts x 2 s) for its read-only registry dependencies to become readable/validated before recursively invoking the Step 36 regression gate.
- Only the three known dependency-read failures are retryable.
- Any non-transient registry blocker prints the registry JSON and fails immediately.
- This preserves STOP-on-failure while avoiding restart races after `--force-recreate`.

