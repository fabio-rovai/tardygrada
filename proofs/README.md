# Tardygrada BFT Consensus Proofs

Formal verification of the Byzantine fault tolerant consensus protocol
used for @hardened and @sovereign agent immutability.

## Protocol

Tardygrada stores immutable values in N replicas. On read:
1. Read all N replicas
2. Hash each replica's value
3. Majority vote on hashes (> N/2 must agree)
4. Verify the winning hash matches the birth hash
5. Return the value from the majority

## Safety Property

If fewer than N/2 replicas are corrupted, the voted value equals the original value.

## Liveness Property

If at least N/2 + 1 replicas are honest, consensus always terminates.

## Verification

```
coqc proofs/consensus.v
```

## Dependencies

- Coq 8.18+ (for the `lia` tactic)
- No external libraries required for the core proof
