# Suspicious track retention — 2026-11-14

## Implemented

- `api/suspicious-track-ledger.js` consumes raw multi-illuminator tracker frames before the existing 64-sample statistics history is pruned.
- Candidate creation is radar-only. Truth is accepted only through the separate post-inference assessment method and is labelled evaluation-only.
- Every retained sample includes channel, track lifecycle, delay, Doppler, measured/coast state, association count, optional confidence/SNR, capture epoch, and half-open capture sample bounds.
- Co-sited carriers are checked for excess-delay agreement and carrier-normalized Doppler agreement. A second transmitter site is recorded as a distinct bistatic geometry, not incorrectly counted as another independent geometry at the same tower.
- The API endpoint is `GET /api/suspicious-track-ledger`; full samples require `?samples=true`.

## Hard bounds

- 4,096 samples per lifecycle
- 512 active and 512 retained lifecycles
- 100,000 total samples
- 128 tracks considered by each once-per-second coincidence scan
- 256 retained events
- 32 MiB persisted file ceiling
- All truncation and eviction paths increment explicit counters

## Validation

`npm run test:statistics` passed in
`/var/tmp/vectorwarp-config-preservation-b7c3b8b` on 2026-11-14. This includes focused lineage, truth-separation, cross-site merge, persistence, sample-overflow, and total-bound tests plus the complete existing statistics suite.

## Boundaries and next acceptance

- No detector, tracker, association, capture, or publication decision is changed.
- No release, commit, push, PR mutation, or live promotion was performed.
- Before internal live activation, verify the target runtime matches this integration worktree, stage the API-only files, restart only the API service, confirm radar frame advancement is uninterrupted, and check that the ledger counters advance while file size remains bounded.
