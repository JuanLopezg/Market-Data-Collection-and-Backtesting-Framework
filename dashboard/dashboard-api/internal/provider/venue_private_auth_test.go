package provider

import "testing"

func TestStep44DeferredPrivateAuthTombstone(t *testing.T) {
	if !step44DeferredPrivateAuthTombstone {
		t.Fatal("deferred account-integration tombstone must remain enabled")
	}
}
