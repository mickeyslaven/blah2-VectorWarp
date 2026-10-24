# Historical ADS-B reconciliation

`script/reconcile-historical-adsb.py` creates immutable, versioned replay
manifest candidates from retained VectorWarp ADS-B samples and an extracted
adsb.lol daily trace archive. It does not edit the input manifests.

For every existing replay frame (normally a 0.5-second grid), the reconciler:

1. uses aircraft position event time, not HTTP receipt or poll time;
2. finds the newest observation at or before the frame from each source;
3. rejects observations older than 30 seconds by default;
4. selects the newest source observation, with retained local data winning an
   exact timestamp tie;
5. recomputes bistatic geometry at the configured receiver/transmitter sites;
6. records all candidates, selected source, age, source event/trace, and
   local-versus-archive disagreement on the frame; and
7. retains the old frame only as `legacy_manifest_v1_fallback` when no raw
   observation is eligible.

No interpolation or averaging is used between sources. Archive Doppler is
derived from successive bistatic path lengths; retained local Doppler is kept
because it was calculated by the live history for that configuration epoch.
Horizontal disagreement above 3 km is flagged as an outlier but does not alter
the deterministic newest-event-time selection.

Example:

```sh
python3 script/reconcile-historical-adsb.py \
  --manifests /root/adsb-history/2026-10-21 \
  --statistics /root/adsb-history/2026-10-21/wwat-statistics-all-epochs.source.json \
  --traces /root/adsb-history/2026-10-21/relevant-traces \
  --output /root/adsb-history/2026-10-21/reconciled-v2
```

The output directory must not already exist. This is a deliberate overwrite
guard. Each candidate embeds source hashes and policy; `validation-report.json`
summarizes per-manifest source coverage, age percentiles, and disagreements.
Operators should retain the daily archive README, extraction inventory, and
SHA-256 inventories beside the candidates as the archive-release audit trail.
