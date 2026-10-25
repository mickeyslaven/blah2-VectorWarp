# Detection-recovery post-merge evidence archive

This note preserves the disposition of five commits added to
`codex/detection-recovery` after pull request #68 merged on 2026-10-24. It is a
historical provenance record, not a current live-radar status report.

## Preserved artifacts

- `results/20261024-detection-manifold-live-deployment.json` records the
  v0.1.28 live deployment identity and the first admitted measured-manifold
  response samples.
- `docs/research/GPS_BELOW_NOISE_PASSIVE_RADAR.md` records the bounded research
  comparison between GNSS integration and ATSC passive-radar processing. Its
  proposals remain replay/shadow experiments, not production-performance
  claims.

## Commit disposition

- `a1df5b3` introduced the first measured detection-manifold implementation.
  Do not reapply it: pull request #74 merged the hardened implementation and
  subsequent fixes.
- `010474b` and `55929bf` added the live deployment receipt, GNSS research note,
  and first admitted corpus observations. Their durable artifacts are preserved
  above; stale current-status prose is intentionally not restored.
- `aa9c20d` documented the then-pending integration-shadow gate. Pull request
  #77 subsequently merged the functional shared-corpus/shadow work, so the old
  operational wording is historical only.
- `7e1d191` clarified the shared array corpus and separate bearing/detection
  models. Equivalent or newer code and documentation merged through pull
  request #77 and the release-integration branch; reapplying it would regress
  later API/UI work.

The original commit objects remain in Git history. No superseded runtime code
from this branch is part of this preservation change.
