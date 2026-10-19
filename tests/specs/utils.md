# Utilities behaviour specification

Purpose: preserve deterministic business dates and configuration, and exchange curve/config
files without silently changing their financial meaning. Sources: architecture business-time
section, `lib/README.md`, public utility headers. These helpers do not execute orders.

R1. Business time is `simulated_reference + speed * (real_now - real_reference)`.
Example: real reference 2024-01-01, simulated reference 2020-01-01, speed 2;
10 real seconds later means 20 simulated seconds later. Speed 1 with zero bias is
identity; speed 1 with bias -24h is exactly yesterday. Tests bracket real reads rather
than waiting or depending on a particular wall-clock date. Technical time stays real.

R2. Speed must be positive and finite; 0, -1, NaN and infinity are errors. A duration
scale outside the clock range must throw rather than wrap (example: max finite speed
multiplied by one elapsed second). Non-positive business waits are already satisfied;
no positive-duration waits are used in these tests.

R3. Unset/empty ALGOTRADING_TIME_* settings yield identity. Nonidentity speed requires
both REAL_REFERENCE_UTC and SIMULATED_REFERENCE_UTC. One reference alone is an error.
Example: speed 2 without references is rejected; speed 1 without references is valid.
Repeated construction with the same settings has the same mapping; no mutable clock
checkpoint exists. Environment mutations in tests must be restored.

R4. References use strict valid `YYYY-MM-DDTHH:MM:SSZ` UTC. Example: real
2024-03-01T00:00:00Z and simulated 2024-02-29T00:00:00Z give bias -86400s.
2023-02-29, hour 24, zone offsets and trailing text are rejected. Speed strings must
be entirely numeric and positive; `2garbage`, `nan`, `inf`, `0` are rejected.

R5. UTC calendar functions preserve day boundaries, leap years and year transitions.
Examples: previous(20240301)=20240229; next(20240229)=20240301;
next(20231231)=20240101; previous(20240101)=20231231.
Unix milliseconds: 19700101=0; 19700102=86400000. Invalid compact dates such as
20230229 and 20241301 throw. Date conversion never depends on local timezone.

R6. The next midnight is strictly later, even at midnight. Example: at
2024-02-29 23:59:59.250 UTC it is 2024-03-01 00:00:00 and the displayed remainder
is 00h 00m 00s (fractional seconds discarded). At midnight the remainder is 24h.
UTC timestamp formatting of 12:34:56.123 is `12:34:56.123 UTC`.

R7. JSON loading preserves numeric values, arrays and booleans. Example:
{"capital":125.5,"enabled":true} keeps 125.5 and true. Missing/malformed files throw
runtime_error. Schema violations throw; for a required nonnegative capital,
125.5 is valid, -1 and omission are invalid. Re-reading a file has no side effects.

R8. CSV curve export preserves row order and the distinction between balance and
equity. The repository's semicolon/comma-decimal export convention is (assumed):
[(100.5,101.25),(99,98.5)] becomes header Index;Balance;Equity and rows
`0;100,5;101,25` / `1;99;98,5`. An empty curve still has a header. Re-exporting the
same path replaces the curve rather than appending duplicate financial rows.
Unopenable output logs failure (void API); it must not create a successful curve.

Invariants/timing: utility conversions cannot expose future bars; callers provide event
time for historical work. Repeated pure conversion/configuration reads are stable;
there are no fills, positions or durable restart counters in this area.

## Findings

No code-versus-spec disagreements found: all 22 behaviour tests passed, with zero
disabled tests. No expected values or specification rules were changed during triage.

Runner anomaly: the first `meson test -C build --no-rebuild --suite unit unit_utils
--print-errorlogs` reported a 30-second timeout even though its captured GoogleTest
output showed all 22 tests passing in 12 ms. Direct execution with
`ALGOTRADING_SOURCE_ROOT=$PWD build/tests/unit/utils/test_unit_utils` exited 0 with
22 passes; a Meson rerun with `--verbose` also exited 0 (1/1 executable, 22 passes).
The timeout's cause was not established; no behavioural failure was reproduced.
