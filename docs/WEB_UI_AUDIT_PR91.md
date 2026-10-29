# Web UI audit and fix ledger — PR 91

Snapshot audited: branch `codex/live-receipt-verification`, starting commit
`19e3929413126969859444c38529d2fa9aa0e990`, including the working-tree fixes
listed below. This audit covers every `html/**/index.html` route and the shared
navigation, status, refresh, plot, setup, and recording behavior. It does not
change receiver tuning, radar DSP, scientific defaults, or deployment state.

## 2026-10-29 completion pass

- Displays is no longer a flat mixed catalog. It is organized as Everyday use,
  Analysis, Advanced signal diagnostics, and a collapsed Developer API.
- The legacy `/display/map` duplicate redirects to the canonical Live radar.
- Detection history has one consistent tab bar across its range, Doppler and
  range–Doppler views instead of three unrelated navigation entries.
- Setup is a sequential six-step workflow. Every Next validates and saves that
  step without applying hardware changes; only Finish applies/restarts. Every
  advertised receiver profile is covered, and unavailable receiver software
  blocks final startup with a direct path to Receiver software setup.
- Experimental array collectors and publication paths have one Advanced-only
  Experimental category instead of being duplicated through ordinary receiver
  and process settings.
- Recording deletion uses a focus-managed dialog. Imports report progress and
  can be cancelled. Replay remains review-first through canonical Settings.
- ADS-B status measures each source's observed publication cadence and marks
  feeds slower than 0.5 seconds as unsuitable for CPI-level pass statistics.
- Aircraft trail state survives browser reloads. Sanitized aircraft-photo
  metadata has a bounded durable API cache and never blocks live ADS-B.
- Released no-op `process.data.overlap`, `process.tracker.smooth`, and
  unimplemented `truth.ais` schema baggage is stripped on read/save and removed
  from fresh and shipped configurations.
- Real Chromium acceptance now checks the primary pages at 320, 375, 768, and
  1440 pixel widths, in addition to the existing mobile radar and map gesture
  suites.
- Overview now includes the beginner health checklist; the separate Health
  route is explicitly the detailed processing/timing diagnostic.
- Detection/tracking scorecards and live ADS-B comparison are tabs in one Radar
  validation workspace. The former ADS-B route is a bookmark-safe redirect.
- The primary Map no longer requests or renders wiring, known-angle, or model
  diagnostics. Those engineering-only panels now live under Array calibration.
- The required DOM suite now exercises repeated live refresh against an open
  details menu, focused table, local table scroll, and page scroll. Map
  interaction/viewport persistence remains covered in real Chromium.

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
| `/controller` | Runtime summary and links use shared bounded status/API behavior; catalog is grouped by user task and the API is collapsed. | Continue hiding receiver-inapplicable routes when runtime capabilities are available. |
| `/display/activity` | Snapshot-preserving refresh; session-only history is disclosed; recording action removed. | Activity overlaps Overview and could become an Overview panel or Advanced view. |
| `/display/calibration` | Initial/empty/error/stale behavior comes from the shared insight controller. | Hide or clearly mark unavailable for non-coherent receivers; the page is status/qualification, not an “assistant.” |
| `/display/configuration` | Guided setup, receiver profile essentials, validation/restart recovery, teardown guard, accessible hidden page heading, and direct tab links verified. | Receiver software cards still have dense expert detail; progressively disclose commands and raw receipts. |
| `/display/detection/delay` | Last valid plot survives failures; visible waiting/error state; no capture controls. | Merge with the other detection history routes as tabs. |
| `/display/detection/doppler` | Same shared plot contract verified. | Merge with Detections tabs. |
| `/display/detection/delay-doppler` | Same shared plot contract verified. | Merge with Detections tabs. |
| `/display/detections` | Valid empty detections are distinct from endpoint failure; snapshot refresh verified. | Prefer “latest radar update” in beginner copy; reserve “CPI” for Advanced/help. |
| `/display/evaluation` | Bookmark-compatible redirect to the live ADS-B tab in Radar validation. | None. |
| `/display/health` | Detailed stage timing, backlog, and source diagnostics; beginner health is consolidated into Overview. | None. |
| `/display/locations` | Map interaction state survives refreshes; ADS-B/tracks/bearing empty and stale behavior covered; capture controls removed; engineering diagnostics removed. | None. |
| `/display/map` | Legacy duplicate redirects immediately to canonical Live radar. | None; retained only as a bookmark-compatible redirect. |
| `/display/maxhold` | Shared non-destructive refresh and visible error state; capture controls removed. | Place under Advanced Signal diagnostics. |
| `/display/overview` | Multi-endpoint refresh is throttled, preserves the last snapshot, reports repeated failure, and includes the beginner system-health checklist. | Recent browser-session Activity remains a separate advanced view. |
| `/display/recordings` | Pagination, search/filter state, indexing refresh ownership, mobile cards, progress/cancel import, focus-managed deletion, and review-first replay verified. | A non-destructive in-browser IQ preview still requires a bounded decimation/streaming API and is not represented as implemented. |
| `/display/site` | Incomplete-coordinate empty state and snapshot recovery verified. | Consider a Site setup tab within Map to reduce navigation fragmentation. |
| `/display/spectrum` | dB scaling, first/late frames, stale/error state, and removal of capture actions verified. | Place under Advanced Signal diagnostics. |
| `/display/statistics` | Radar validation workspace combines portable/rich scorecards and live ADS-B comparison as explicit tabs. | None. |
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

1. Add automated accessibility semantics beyond the current real-browser
   viewport, interaction, overflow, and JavaScript-error checks.
