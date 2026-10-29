# Web UI audit and fix ledger — PR 91

Snapshot audited: branch `codex/live-receipt-verification`, starting commit
`19e3929413126969859444c38529d2fa9aa0e990`, including the working-tree fixes
listed below. This audit covers every `html/**/index.html` route and the shared
navigation, status, refresh, plot, setup, and recording behavior. It does not
change receiver tuning, radar DSP, scientific defaults, or deployment state.

## Cross-route fixes in this audit

- Raw-IQ start/pause/stop is now opt-in and appears only on the canonical Live
  radar route. Analysis, history, timing, map, and diagnostic charts no longer
  gain a destructive recording action or a four-second capture-status poll just
  because they contain an element named `data`.
- Plot pages expose accessible Loading, Waiting for first frame, repeated Error,
  and stale-last-valid-frame states. A refresh failure does not erase the most
  recent valid plot.
- Mobile keeps radar/ADS-B operational status visible and allocates five grid
  columns for the five primary navigation links.
- Primary navigation uses `aria-current`; dynamic service states are live
  regions; plot headings remain available to assistive technology.
- Recording Replay now opens Settings with the file preselected for review. It
  no longer writes configuration and initiates a restart through a second,
  short-timeout workflow.
- Recording deletion catches network failures, restores its button, preserves
  the library, and reports a recoverable in-page error.
- Recording settings deep-links to the Recording tab. Settings honors validated
  `?tab=` links without allowing arbitrary selectors.
- Simple/Advanced and setup-step selection expose `aria-pressed` and
  `aria-current`; the setup dialog has an accessible name.

## Route ledger

| Route | Feature/function audit outcome | Remaining organization/readability issue |
| --- | --- | --- |
| `/` | Canonical live delay–Doppler; real loading/empty/error/stale state; only route with capture controls; accessible heading added. | Keep marker legend and beginner explanation synchronized with plot semantics. |
| `/controller` | Runtime summary and links use shared bounded status/API behavior. | Still a long flat catalog. Collapse Data API under Developer; group signal diagnostics separately from beginner views. |
| `/display/activity` | Snapshot-preserving refresh; session-only history is disclosed; recording action removed. | Activity overlaps Overview and could become an Overview panel or Advanced view. |
| `/display/calibration` | Initial/empty/error/stale behavior comes from the shared insight controller. | Hide or clearly mark unavailable for non-coherent receivers; the page is status/qualification, not an “assistant.” |
| `/display/configuration` | Guided setup, receiver profile essentials, validation/restart recovery, teardown guard, accessible hidden page heading, and direct tab links verified. | Receiver software cards still have dense expert detail; progressively disclose commands and raw receipts. |
| `/display/detection/delay` | Last valid plot survives failures; visible waiting/error state; no capture controls. | Merge with the other detection history routes as tabs. |
| `/display/detection/doppler` | Same shared plot contract verified. | Merge with Detections tabs. |
| `/display/detection/delay-doppler` | Same shared plot contract verified. | Merge with Detections tabs. |
| `/display/detections` | Valid empty detections are distinct from endpoint failure; snapshot refresh verified. | Prefer “latest radar update” in beginner copy; reserve “CPI” for Advanced/help. |
| `/display/evaluation` | ADS-B comparison explicitly states that it does not drive radar; shared recovery verified. | Combine with historical Statistics under a Radar validation concept. |
| `/display/health` | Current/stale/error states and last valid snapshot verified. | Overview duplicates several health metrics; combine beginner summary and keep stage timing expandable. |
| `/display/locations` | Map interaction state survives refreshes; ADS-B/tracks/bearing empty and stale behavior covered; capture controls removed. | Move known-angle/wiring/model diagnostics to Calibration/Advanced. |
| `/display/map` | Legacy duplicate gets shared plot state and no unintended capture action. | Redirect to `/`; maintaining two “Live delay–Doppler” URLs invites drift. |
| `/display/maxhold` | Shared non-destructive refresh and visible error state; capture controls removed. | Place under Advanced Signal diagnostics. |
| `/display/overview` | Multi-endpoint refresh is throttled, preserves the last snapshot, and reports repeated failure. | Merge the beginner portion of Health and recent Activity here. |
| `/display/recordings` | Pagination, search/filter state, indexing refresh ownership, mobile cards, import state, delete recovery, and review-first replay verified. | Replace native `confirm` with a focus-managed destructive-action dialog; add import progress/cancel when transport support exists. |
| `/display/site` | Incomplete-coordinate empty state and snapshot recovery verified. | Consider a Site setup tab within Map to reduce navigation fragmentation. |
| `/display/spectrum` | dB scaling, first/late frames, stale/error state, and removal of capture actions verified. | Place under Advanced Signal diagnostics. |
| `/display/statistics` | Portable/rich statistics smoke coverage and snapshot recovery remain intact. | Rename from generic “Performance” to Radar validation/Tracking scorecard and pair with ADS-B evaluation. |
| `/display/timing` | Shared frame cadence and visible failure state; capture controls removed. | Advanced/Developer view, not a peer of beginner radar plots. |
| `/display/tracks` | Empty/current/stale snapshots use shared insight behavior; recording action removed. | Add concise explanations for tentative, active, coasting, and confirmed state terms. |

## Verification

- `npm run test:dom` — passes, including all 21-route refresh/audit contracts.
- `node test/ui/common.test.js` — passes.
- `node test/ui/frame-updates.test.js` — passes.
- `node test/ui/plots.test.js` — passes.
- `node test/ui/branding.test.js` — passes.
- `node test/ui/deployment.test.js` — passes.
- JavaScript syntax checks pass for `common.js`, `recordings.js`, and
  `setup_assistant.js`.

## Prioritized remaining work

1. Redirect `/display/map` to `/`, then consolidate Detections history routes.
2. Reorganize Displays into Beginner, Signal diagnostics, and Developer; hide
   receiver-inapplicable entries instead of presenting dead-end features.
3. Merge Overview/Health beginner content and ADS-B/Statistics validation content.
4. Move bearing engineering diagnostics out of the main Map experience.
5. Add a focus-managed recording deletion dialog and upload progress/cancel.
6. Add real-browser visual regression and automated accessibility checks at
   320, 375, 768, and desktop widths; current regression coverage is DOM and
   behavior oriented rather than pixel comparison.
