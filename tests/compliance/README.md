# Compliance Test Harness

Measures Tardygrada's ability to distinguish correct, wrong, and plausible-but-wrong financial compliance statements across different verification thresholds.

## What it measures

The test suite contains 100 statements across three categories:

- **40 CORRECT** statements about financial regulations (Basel III, SOX, GDPR, AML, etc.) that should be accepted when an ontology is connected
- **30 WRONG** statements that are clearly false and should always be rejected
- **30 PLAUSIBLE-BUT-WRONG** statements that sound reasonable but contain incorrect details (wrong numbers, wrong dates, wrong scope) — the critical category for measuring selectivity

## How to run

Quick run (10 statements, suitable for CI):

```bash
python3 tests/compliance/run_compliance.py --quick
```

Full run (all 100 statements):

```bash
python3 tests/compliance/run_compliance.py --full
```

Options:

- `--verbose` / `-v`: Print per-statement detail tables
- `--json-output FILE`: Write structured results to a JSON file

## Two test phases

### Phase 1: No ontology

All statements are submitted without an ontology connected. The expected result is that **every statement is rejected** with UNKNOWN grounding. This proves Tardygrada is honest when it lacks knowledge — it never hallucates acceptance.

### Phase 2: Mock ontology (simulated)

Simulates what happens when a real ontology is connected:
- Correct statements get `grounded` status with high confidence
- Wrong and plausible-but-wrong statements get `contradicted` status

This shows the selectivity curve that a real deployment would produce.

## How to interpret results

### Selectivity curve

The output table shows acceptance rates at three thresholds:

| Threshold | Min Confidence | Trade-off |
|-----------|---------------|-----------|
| LOW       | 0.85          | Accepts more correct statements, but higher false acceptance risk |
| MEDIUM    | 0.95          | Balanced — good for most compliance use cases |
| HIGH      | 0.99          | Very selective — may reject some correct statements, but minimal false acceptance |

### Key metrics

- **FAR (False Acceptance Rate)**: Percentage of plausible-but-wrong statements that passed verification. Lower is better. In compliance, this is the most dangerous failure mode.
- **TRR (True Rejection Rate)**: Percentage of clearly wrong statements that were correctly rejected. Should be 100% or very close.

### What good results look like

Without ontology:
- 0% acceptance across all categories and thresholds (honest fallback)

With ontology:
- Correct acceptance > 90% at LOW threshold
- FAR = 0% at all thresholds
- TRR = 100% at all thresholds

## Statement domains

The 100 statements cover:
- Banking regulations (Basel III, capital requirements, leverage ratios)
- Securities law (SEC reporting, insider trading, Reg FD)
- Anti-money laundering (BSA, KYC, SARs, PATRIOT Act)
- Corporate governance (SOX, auditing, certifications)
- Consumer protection (CFPB, GLBA, FCRA)
- Data protection (GDPR intersecting with financial services)
- International frameworks (FATF, MiFID II, FATCA)
