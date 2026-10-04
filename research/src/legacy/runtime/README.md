# Frozen legacy research runtime

Purpose: compile the historical research executables without changing the
modern Strategy/SignalState architecture used by canonical replay/live code.

Historical core source:
`89eb80cb440d44c31a762c402074871d56610190`

The breakout/older strategy headers were local/ignored files in this working
tree and therefore do not exist in Git history. Copies are frozen here for
legacy research only.

`pureRSI.h` in this runtime is a compatibility implementation for the old
Strategy API. The production/current PureRSI is not modified.

Do not use this runtime from canonical replay, dashboard, TESTNET or LIVE.
