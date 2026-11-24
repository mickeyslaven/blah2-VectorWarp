# Browser acceptance

`all-routes-layout.cjs` is the canonical Chromium/CDP gate. It serves the shipped
HTML and JavaScript from a loopback-only fixture, sweeps every canonical/query
route and compatibility URL at six viewport sizes, and checks layout and assets.
With `VECTORWARP_LAYOUT_INTERACTIONS=1`, it also clicks desktop/mobile navigation,
receiver/profile settings, valid/invalid saves and reloads, channel extraction,
station paste/file import and confirmation, location dialogs, setup/error recovery,
migration acknowledgement/history, and node add/remove/cancel/retry flows.
An inventory discovers the rendered editable controls across representative
receiver, array/resident-capability, and channelized fixtures. Each available
control is focused, changed and restored; validation and newly enabled dependents
are checked. Wizard, site and node forms are included. The JSON report lists
visited controls and explicit unavailable/single-choice reasons, and unvisited
editable controls fail the gate. This is a union of capabilities, not an exhaustive
Cartesian browser matrix; DOM/native parity tests cover those combinations.
The same interaction run inventories visible settings buttons, disclosures and
links. Existing scenario clicks count toward coverage; additional cases exercise
draft-only array edits/presets, ADS-B source rows, fixture-local recording folders,
direct Save, and diagnostic preview/copy/cancel. Disabled capability actions,
external support links and the explicit report-submission privacy gate are
recorded separately; unexplained actions fail. No report is submitted and no
hardware/system action is executed by this fixture.

After installing the API dependencies and Chromium, run from the repository root:

```sh
VECTORWARP_CHROME_EXECUTABLE=/path/to/chromium \
VECTORWARP_LAYOUT_INTERACTIONS=1 \
VECTORWARP_LAYOUT_REPORT=build/browser-acceptance/browser-layout.json \
VECTORWARP_LAYOUT_SCREENSHOTS=build/browser-acceptance/screenshots \
node test/browser/all-routes-layout.cjs
```

Linux and both macOS CI jobs require the full route and interaction matrix with a
15-minute limit and retain the JSON report and screenshots for 14 days, including
failure evidence. `test/packaging/test_ci_workflows.py` checks that contract and is
part of prebuild acceptance. No pixel snapshots are silently updated: interactions,
focus, visible geometry, page errors and missing assets determine acceptance;
screenshots support human review.

Those jobs also run `fusion-responsive.cjs` and `statistics.cjs` before the route
matrix. They retain fusion kiosk screenshots in the same evidence artifact. The
fusion test launches its own disposable Chromium with `--remote-debugging-port=0`
and discovers that test process's endpoint.

The setup fixture uses validated, synthetic configurations and memory-only writes.
It never installs drivers, starts receivers, contacts peers, or changes operational
configuration. OS/receiver fixtures test UI capabilities, not RF, GPU performance,
or native driver qualification; those still require the native/runtime gates.
