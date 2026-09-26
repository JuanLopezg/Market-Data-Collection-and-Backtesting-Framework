# Step 42 fix 0.42.2 — tolerate only transient SymbolRegistry WARN sampling

Observed failure: Step 42 invoked the Step 37A regression gate after the current
`/api/symbol-registry` endpoint had already validated, but `/api/alerts-audit`
performed an independent read and briefly sampled a whitelisted read-only
dependency timeout. The provider correctly downgraded that derived
`SYMBOL_REGISTRY_BLOCKED` condition to `WARN` while order routing is disabled,
but the Step 37A shell gate rejected the event type regardless of severity.

Fix: the Step 37A Alerts & Audit gate now fails only when
`SYMBOL_REGISTRY_BLOCKED` is `CRITICAL`. A `WARN` instance is accepted only as
observability of the already-whitelisted transient dependency-read condition;
the same run must first have validated `/api/symbol-registry` and Step 36.

No safety downgrade:
- registry/artifact/manifest failures remain CRITICAL;
- unregistered or divergent symbols remain CRITICAL and non-routable;
- venue-absent / partial coverage remain WARN and non-routable;
- order routing and manual routing remain disabled;
- no wallet, signing, `/exchange`, submit or cancel surface is added.
