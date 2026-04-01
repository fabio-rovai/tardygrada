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

Coq has been renamed to the **Rocq Prover** (as of 2024). Both `coqc` and `rocq`
work to compile proof files.

### Install via Homebrew (macOS)

```bash
brew install rocq
```

This pulls in OCaml, GMP, and other dependencies (~500 MB). Once installed:

```bash
coqc proofs/consensus.v
```

A successful compilation produces `proofs/consensus.vo` with no output — silence
means all proofs check.

### Install via opam (Linux / other)

```bash
opam install rocq-prover
eval $(opam env)
coqc proofs/consensus.v
```

## Dependencies

- Rocq (Coq) 8.18+ (for the `lia` tactic)
- No external libraries required for the core proof
