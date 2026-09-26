# Step 37 — Hyperliquid TESTNET API-wallet credential + authorization

Status: implementation prepared; acceptance requires a real local Step 37 gate run.

Step 37 introduces no order route and no Hyperliquid `/exchange` request. It verifies: the TESTNET account role (`user`; subaccount signing is deferred until its master/agent relationship is modeled explicitly); that the configured API-wallet address is an `agent` authorized for that account; and that a local private-key file outside dashboard containers derives to that exact agent address and can perform a generic local secp256k1 sign/verify self-test.

Add these PUBLIC addresses to `deploy/live/.env`:

```dotenv
HYPERLIQUID_TESTNET_ACCOUNT_ADDRESS=0x...
HYPERLIQUID_TESTNET_API_WALLET_ADDRESS=0x...
```

Store only the API-wallet/agent private key at:

```text
~/.config/algotrading/secrets/hyperliquid-testnet-api-wallet.key
```

The file must contain one 32-byte hex private key (optional `0x` prefix) and should be mode `0600`.

Never place a master-wallet private key, seed phrase, recovery phrase, or any private key in `deploy/live/.env`, dashboard compose, dashboard source, or browser storage.

Boundaries preserved: ExchangeGateway remains `hyperliquid-dry-run`; no `/exchange`; no Hyperliquid L1/EIP-712 action signing; no submit/cancel; Manual Control fail-closed; no real capital.

Acceptance:

```bash
./scripts/step37-private-auth.sh
```


Important acceptance semantics:

- `userRole` is public authorization evidence; it proves the configured agent is registered for the configured TESTNET account.
- The local key derivation proves possession of the matching API-wallet private key without sending it anywhere.
- Step 37 does not prove Hyperliquid protocol signing. L1/EIP-712 signing is intentionally deferred because Hyperliquid has distinct signing schemes and the production implementation should use/reproduce the official SDK semantics exactly.
- No subaccount support is claimed in this step.
- Do not paste the API-wallet private key, master private key, seed phrase, or recovery phrase into chat, `.env`, dashboard source, or browser storage.
