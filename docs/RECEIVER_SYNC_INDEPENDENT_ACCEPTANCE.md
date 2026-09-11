# Receiver synchronization independent review — 2026-09-10

Reviewed external handoff: `/tmp/vectorwarp-receiver-sync-20260910`, based on
`9cfc783`. Remediation is isolated in
`/tmp/vectorwarp-receiver-sync-reviewed-20260910`, based on committed geometry
integration `b870658`. Neither the external handoff nor shared checkout was
modified by this review. No actual receiver endpoint, service, hardware,
installer, signing key or benchmark was used.

## Reproduced blockers and fixes

The independent loopback suite initially passed **1/9** gates. Eight failures
were reproduced: status received before ACK accepted as readback; sample-rate,
mode, count and reconfiguration drift accepted during frequency readback;
completed count allowed to regress during final readback; replay-to-live
transition skipped; changed IQ input port skipped.

The protocol now requires the exact command ACK followed by a later status.
Each operation checks a coherent, non-reconfiguring tuple with the requested
sample rate and active count. The count operation checks the original frequency
before retuning; the frequency operation and final completion check the full
requested tuple. Unexpected intermediate drift cannot trigger the next command.
Changing the IQ port or leaving replay requires a fresh synchronization attempt.

Server integration preserves the geometry release's canonical saved-config
response. Applied commands clear draft geometry assertions before a successful
save, even when reconciliation returns to the unchanged saved target. Every
production live-Kraken save, including Save + Restart, requires fresh full-state
readback under the explicit synchronization intent header, even after an API
restart and with no YAML diff. Already-matched state sends no commands and
neither promotes nor clears operator assertions merely because status matches.
The volatile failure receipt is diagnostic, not the reconciliation safety gate.
YAML stays unchanged on command/readback failure. Persistence conflict/failure
remains distinct, carries the receiver receipt, and never triggers automatic
hardware rollback.

The browser explicitly reports partial/indeterminate receiver outcomes instead
of reducing them to a generic save error. Such outcomes clear draft geometry
assertions while preserving measurements; Discard cannot resurrect those
assertions during that editor session. Lost transport produces “save outcome
unknown,” not an assertion that the server did nothing. The write timeout is
90 seconds, covering the maximum configured 10-second initial observation,
two 30-second operation deadlines and bounded preflight/response overhead.
Replay changes also invalidate the geometry context. No serial-order,
calibration, physical readiness or actual bearing verification is claimed.

## Validation

Final tests used existing `blah2-ui-config-preview`, capped at 0.5 CPU, CPUs
8/24, 8 GiB memory and equal memory/swap ceilings. Source was a disposable
snapshot at the same isolated path inside the container. The exact packaged
Node 24.21.0 binary was selected through
`PATH=/tmp/vectorwarp-packaged-runtime.wy0785:...`;
`NODE_PATH=/tmp/vectorwarp-api-tests.giv83Q/api/node_modules` supplied the freshly
installed dependencies, including qs 6.16.0 and jsdom 22.1.0.

`npm test` and `npm run test:dom` passed, including original protocol tests,
all **9/9** independent adversarial gates, real disposable API/loopback tests,
geometry persistence and browser tests, all 32 prior geometry adversarial cases,
and branding/common/config regressions. The server tests cover actual disposable
API-child restart after a partial receiver transaction, unchanged saves with
sample-rate/mode/reconfiguration mismatch, no-command matched-state saves,
Save + Restart refusal on mismatch, and replay-to-live synchronization.
`git diff --check` passed. No rendered-browser or physical-hardware acceptance
is claimed.

The existing SDK source-contract test passes its assertions but explicitly
reports unmet RSPduo runtime acceptance: abrupt `exit(1)` paths can prevent
structured failure telemetry, and rxChannelA assignments do not establish
vendor-backed dual-tuner mirroring. Those direct-receiver limitations are
unchanged and outside this Kraken transaction remediation.

## Remaining boundaries

- Receipt history is in memory, with no durable transaction journal. Restart
  cannot bypass the next API save's mandatory readback, but external service
  starts are outside this API route: there is no processor-start or continuously
  enforced IQ/config agreement gate. Stored assertions remain unverified.
- Separate browser sessions do not share the current editor's sticky assertion
  invalidation. Status receipts are observational; no runtime physical mapping
  can be proven while serial order and survey provenance are unreported.
- A successful later Suite status confirms reported configuration only. It does
  not bind an IQ stream to that status, prove physical cabling/coherence, or make
  bearing available. No automatic rollback, sample-rate setter, serial reorder,
  reference-role rewrite or Suite DoA geometry transfer was added.

Merge only source/test/report changes. Do not include generated patch artifacts
or `review-base-config_ui.js`. Preserve the newer shared branding/release/Pi docs
and the geometry canonical-response hook. The external handoff's unrelated
README/install/default-config wording changes are not included in this snapshot.
