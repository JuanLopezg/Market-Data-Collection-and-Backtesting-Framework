# Step 44 fix 0.44.6

The Step 44 safety scan previously searched arbitrary provider text for the substring `/exchange`.
That produced false positives from descriptive source-contract strings and from a historical Step 37 prototype file left behind when a cumulative ZIP was extracted over an existing tree.

Fix:
- Audit the actual HTTP route registry in `internal/server/server.go`.
- Allow only the pre-existing mutating routes for auth and `manual-control/preview`.
- Fail on any registered trading command route.
- Continue scanning compiled Go sources for secret-bearing runtime identifiers.
- Ship an inert `venue_private_auth.go` tombstone to overwrite the historical deferred prototype when extraction happens over an older tree.
- Private venue integration remains deferred; no signing/order route is enabled.
