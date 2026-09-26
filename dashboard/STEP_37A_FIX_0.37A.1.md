# Step 37A fix 0.37A.1 — cumulative-upgrade compatibility

## Observed failure

A local Docker build failed in `internal/provider/venue_private_auth.go` because
that Step 37 read-only probe file was still present from an earlier extraction,
while the Step 37A cumulative tree had removed the two corresponding public
address fields from `provider.RealConfig`.

The failure was a stale-file compatibility issue, not a failure of the Step 37A
symbol registry itself.

## Fix

`provider.RealConfig` once again carries the two **public-address-only** fields:

- `VenueAccountAddress`
- `VenueAPIWalletAddress`

They are retained only so a local tree that still contains the deferred Step 37
read-only authorization probe can compile cleanly. Step 37A does not require,
consume or validate those values and does not accept any private key.

This does **not** enable Step 37, private auth, signing, `/exchange`, submit,
cancel or order routing.

## Upgrade rule

The preferred installation remains replacing the whole `dashboard/` directory.
This fix additionally tolerates the known stale Step 37 probe files if an
archive was accidentally extracted over the previous folder instead of replacing
it.

## Validation

Validated both ways:

1. clean Step 37A.1 tree;
2. simulated polluted tree containing the Step 37-only probe/test/script files.

Both pass `CGO_ENABLED=0 go test ./...`, `go vet ./...` and `go build ./...`.
