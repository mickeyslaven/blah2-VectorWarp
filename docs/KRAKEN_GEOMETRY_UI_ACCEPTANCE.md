# Optional Kraken antenna-layout recording — 2026-09-10

Shared-source integration completed in `/opt/blah2-kraken-2to8-ui-config` after
the isolated review. All changes were applied without staging or committing.
The existing seven receiver-route additions in `api/server.js`, all receiver
modules, `common.js?v=vw-logo-1` and the npm branding test were preserved. The
reviewed pure geometry module/tests/reports were not overwritten; their shared
hashes are unchanged (module SHA-256
`9d2a0b19a98d67eb23a60decf4f4f172a2056491c0b5f9a7997a9ac1850fa0d4`).

Merged-source acceptance passed in `blah2-ui-config-preview`, working from
the shared read-only `/src/api` mount, using
`NODE_PATH=/tmp/vectorwarp-api-tests.giv83Q/api/node_modules`:
`npm test`, `npm run test:dom`, and
`node ../test/ui/kraken-geometry-layout.test.js` all exited successfully.
Dependency resolution explicitly confirmed qs 6.16.0 and jsdom 22.1.0.
Fresh CSS/DOM-only evidence is in container directory
`/tmp/kraken-geometry-layout-evidence-XS3inO`. No generated fixture or patch
artifact was copied into the shared Git tree. Full rendered-browser visual
verification remains unavailable. Container limits remained 0.5 CPU, CPUs 8/24,
8 GiB memory and an equal memory/swap ceiling; no live settings or services
were changed.

Scope: source-only integration on detached worktree
`/opt/blah2-kraken-geometry-ui-20260910`, based on `7a9244f` from
`review/vectorwarp-native-integration-20260910`. No commit, deployment, receiver
command, service restart, hardware action, model work or system-limit change.

`html/js/kraken_geometry.js` provides the optional Receiver-section editor and
the bounded metadata schema shared by the API. Minimal hooks in
`config_ui.js`, `config-manager.js`, `config-store.js` and `server.js` connect
editing, validation and canonical save/reload. A separate stylesheet contains
the element-grid layout. `html/display/configuration/index.html` loads both.

The independent reviewed `api/kraken-array.js` and its original/adversarial
tests were copied unchanged in logic. Its eligibility flags remain false.
No Suite DoA geometry, selected IQ, inference/tracker, ADS-B or bearing output
path was added. The UI says operator-recorded, physical agreement unverified;
live serial order, cabling and survey freshness remain unknown.

## Record behavior

- Optional `capture.device.array_geometry`; legacy omission does not change
  passive-radar validity. All 2–8 channel counts and every dedicated-reference
  position retain support, including two total inputs / one surveillance input.
- Draft records may have null coordinates, unassigned DAQ channels or a partial
  bearing subset. Units, data types, safe finite numeric bounds, maximum eight
  elements, unique assigned DAQ/serial/element identities and bounded metadata
  are enforced. Incomplete or role-conflicting records produce review notices,
  not a claim that ordinary passive radar is unavailable.
- Custom, cross, ULA and regular-polygon guides keep measurements and channel
  roles. New rows have blank coordinates, unknown serials and unassigned DAQ
  channels. Templates never generate measurements or survey confirmations.
- Five physical pentagon vertices with one dedicated reference classify the
  four-bearing subset as custom. Five bearing elements plus a separate reference
  need six channels. The four-arm cross reference may occupy any explicit DAQ
  channel. UI role text is derived only from the operator's radar configuration.
- Every count, frequency, connection, role, synthesis, element order, DAQ/serial,
  coordinate, shape, subset and orientation change clears operator statements.
  A count decrease/reincrease retains all measured rows without restoring prior
  statements. Explicit Remove element controls remove only the selected row and
  its bearing selection. Discard changes remains available before saving.
- Switching receivers retains the record as dormant metadata. Only Kraken shows
  the full editor; another profile shows a compact dormant-record removal action.
  A later Kraken selection retains measurements with confirmations
  cleared. Bounded unknown record/element metadata survives save and reload.
- The store independently clears statements when candidate context differs from
  the saved context. Users save measured changes before adding their statements
  to that saved context. The API returns canonical saved config; the browser
  adopts it so visible confirmation values match the file immediately.
- Statements remain operator assertions, with no live physical or acquisition
  verification. Unreported serial order and acquisition freshness cannot enable
  bearing. Known/unknown upstream statuses never affect geometry eligibility.
- Unsupported saved dropdown values remain visible with a clearly unsupported
  option and native validity error. Rendering never substitutes a supported
  value. Malformed records expose a confirmed draft removal action for Kraken
  and other profiles, so an invalid dormant record cannot trap the form. Cancel
  retains every value; Discard restores the saved record until an explicit Save.

## Validation

All checks run in existing `blah2-ui-config-preview`: 0.5 CPU, CPUs 8/24,
8 GiB RAM and equal memory/swap ceilings. Isolated source snapshot was copied
to `/tmp/kraken-geometry-ui-20260910` inside that container; the original `/src`
mount remained read-only and unchanged. `NODE_PATH` used
`/tmp/vectorwarp-api-tests.giv83Q/api/node_modules` (Node 16, jsdom 22 and locked
qs 6.16.0). Temporary API processes, config files and ports are test-local.

Passed original pure geometry suite and all 32 independent adversarial cases;
new `api/kraken-geometry-config.test.js`; new real disposable API/YAML/browser
`test/ui/kraken-geometry-dom.test.js`; existing config-manager, config-recovery,
config-server, config-ui and config-dom suites. The new tests cover schema and
bounds errors, all reference positions across 2–8 channels, each layout shape,
inactive measurements, count/role/permutation invalidation, dormant profile
  retention, unknown fields, save/reload, stale API revisions and status isolation.
Default API tests include the geometry/schema suites; `test:dom` includes the
  new browser integration; `test:geometry` runs all geometry gates explicitly.

## Limited layout acceptance follow-up

No rendered-browser tool, Chromium/Chrome or Firefox executable was available
in the existing capped container or the host. No browser dependencies were
installed. Consequently there is **no screenshot or rendered-pixel acceptance**.

CSS inspection found a concrete inherited sizing conflict: `.config-field`
specified two columns with 240px and 220px minima, plus gap and padding, while
the geometry grid offered cells as narrow as 150px. The scoped geometry CSS now
uses one shrinkable column inside each element cell, border-box controls with
`min-width:0` and `max-width:100%`, wrapping labels/errors/buttons, explicit
checkbox sizing, and responsive element tracks capped by their container width.
Desktop top-level controls retain a separate two-column arrangement. Text was
shortened to one common record-only notice and brief field hints.

`test/ui/kraken-geometry-layout.test.js` passed as a CSS/DOM fallback only. It
checks eight-element records, labels, long serial values, an unsupported shape
dropdown and visible error text with the actual combined CSS. It asserts the
computed one-column element-field rule, bounded control styles, checkbox sizing
and error wrapping. Fixtures are prepared for 1440px, 390px and 320px inspection;
jsdom does not calculate layout or provide responsive pixel measurements.

Evidence generated in the capped container:
`/tmp/kraken-geometry-layout-evidence-5ddokz/evidence.json` and
`geometry-8ch-{1440,390,320}-css-dom-fixture.html` in the same directory.
The fixture files include the CSS and DOM for later real-browser inspection.
Copies are available in the isolated worktree's `visual-evidence/` directory.
New DOM regressions cover unsupported dropdowns, invalid-field error labels,
malformed records, dormant-profile recovery, confirmation cancel/accept, Discard
and actual API/YAML removal. The geometry, config-UI and browser-DOM regressions
were rerun after these fixes.

Remaining acceptance: visual review of the merged Receiver layout, real physical
survey and runtime serial/acquisition provenance. No physical wiring, survey,
phase calibration, Suite coordinate adapter, selected-IQ covariance, bearing
implementation or deployment is accepted by these source tests.

## Merge boundaries

Bring over the new geometry helper, stylesheet, two integration tests and this
report. Merge small edits in config-manager/config-store/config_ui/server and
the package scripts. Insert the stylesheet/helper script into the current
Settings HTML without replacing concurrent branding changes. The pure module,
its original/adversarial tests and two prior acceptance reports already exist
in the shared source; keep those reviewed shared copies.
