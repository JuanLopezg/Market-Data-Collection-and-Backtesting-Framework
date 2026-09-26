# Step 45 fix 0.45.3

The REAL endpoint can legitimately have too little completed canonical history to classify live-vs-expected behaviour. This fix separates **contract validation** from **projection readiness**.

- `status=VALIDATED_LIMITED` + `validated=true` means the read-only Step 45 contract is structurally valid.
- `projectionReady=false` + `overallClassification=INSUFFICIENT_DATA` means there are fewer than five independent baseline observations; no metric classification is fabricated.
- Market-input coverage reports `INSUFFICIENT_DATA` rather than a semantic/source failure.
- The gate accepts this honest fail-closed state and still rejects fabricated metrics, unexpected classifications, unsafe routing/auth changes, or an unready projection once enough baseline observations exist.
- Full READY behaviour is unchanged once six completed observations exist (five baseline + latest).

No wallet, signing, submit/cancel, private auth, or order-routing surface is introduced.
