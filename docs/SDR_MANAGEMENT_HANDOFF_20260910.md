# SDR management integration handoff — 2026-09-11

Historical handoff, not current setup instructions. The integration and
package-policy work described as pending here has since changed; follow
[receiver setup](SETUP.md), [current acceptance](UPSTREAM_COMPARISON.md) and
[release packaging](../packaging/README.md).

Worktree `/tmp/vectorwarp-sdr-management-20260910.BHprGN`, branch
`codex/vectorwarp-sdr-management-20260910`, base `ee275eb27939ff7c18479089ec6a889efdc73383`.
All files in this worktree belong to the scoped SDR change. The dirty shared
checkout was not touched. The parent owns integration, publishing, real hardware
and screenshots; this agent performed source work and bounded offline acceptance.

## Delivered source

- Settings checks all four compiled/detected/dependency profiles, correlates
  configured serials and preserves remote Suite reuse. Missing compiled backends
  get build guidance, never a promise that installing an SDK will enable them.
- Explicit Save for later writes pending YAML without receiver commands or
  restart. Apply uses ACK followed by fresh complete Kraken readback even for
  unchanged live settings. An atomic, fsynced write-ahead journal survives API
  death, preserves unknown receipts and blocks pending saves until reconciliation.
  Its group is shared with the separate processor account.
- Packaged processor startup performs read-only journal/config validation and
  fresh Kraken control readback. Pending/unresolved/revision-drift states block
  live startup. Direct backends retain SDK startup validation; replay is exempt.
  Existing Kraken IQ ingress checks each frame's channel count, frequency and
  synchronization. No independent physical RF/calibration proof is claimed.
- Root broker uses peer OS credentials, exact installed artifact hashes, local
  terminal approval, expiring one-use plans, API-origin/revision/nonce checks and
  transaction exclusion. Default policy remains empty. An administrator can
  enroll only fixed allowlisted existing Kraken Suite/SDRplay services; enrollment
  pins unit/executable/build hashes and does not start them. Browser starts require
  a separate one-use approval. Existing active software is reused.
- Native add-only dependency adapters are executable for Ubuntu/Debian APT and
  Fedora 44 DNF5. DragonOS qualifies only as an Ubuntu derivative with every
  archive authenticated as native Ubuntu. Complete dependencies, exact versions,
  architecture, archive hashes, native origins and package database revision are
  reviewed locally and displayed in the browser. Native manager locking spans
  fresh resolution through execution/postconditions. No upgrades, removals,
  arbitrary packages, shell commands, third-party repositories or RPM key imports.
  Limits: 64 packages, 512 MiB downloads, 180 seconds per execution, no retry of a
  consumed grant. UHD requires 4.8+. Unsupported distributions/APIs or necessary
  existing-package changes return actionable refusal, not success.
- Debian packages depend on python3-apt; Fedora44 RPMs depend on matching native
  python3-libdnf5/python3-rpm. Both workers are packaged beside the root broker.
  SDRplay's proprietary API remains an explicit vendor-license/local-install step.
- RSPduo reports structured exceptions, stages cleanup and configures both tuners;
  UHD getter readback gates startup; HackRF validates paired inputs and releases
  its descriptor inventory. Receiver settings are not independent RF evidence.

## Verified acceptance

Existing containers retain 0.5 CPU on CPUs 8/24 and 8 GiB; no actual package
installation, driver change, receiver/service action or physical vendor test ran.

UI snapshot `/tmp/vectorwarp-sdr-final-20260911` in `blah2-ui-config-preview`:
full API suite, all DOM suites, management flow, native packaging and nine staging
tests (one CMake case skipped in this Node-only environment). Management includes
17 broker, 9 APT and 11 DNF failure-injection tests; restart/journal tests use real
isolated APIs and simulated loopback Suite endpoints. Final runner:

```sh
npm test --prefix api
npm run test:dom --prefix api
npm run test:receiver-management --prefix api
node test/ui/packaging.test.js
python3 test/packaging/test_receiver_build.py
```

Fresh all-vendor native configure in `blah2-gpu-build` at
`/tmp/vectorwarp-sdr-final-native-build-20260911` built and passed both
`usrpAppliedReadback` and `rspduoStructuredFailures` CTest registrations. These
fixtures use fake SDK operations, no vendor runtime or device.

Real read-only package API checks:

- Ubuntu22.04 python3-apt2.4: missing HackRF resolved to three native signed
  packages with exact hashes. No package was downloaded/installed by the adapter.
- Fedora44 libdnf5/python bindings5.4.4.0: existing HackRF2026.01.3 and UHD4.9.0.1
  reused. A private empty RPM database exercised the actual missing-HackRF
  resolver: 19 packages, 10,076,597 download bytes, complete SHA-256/native origins;
  no installation. See optional `test/packaging/receiver_dnf_api_smoke.py`.

Official binding packages were downloaded/extracted only inside the bounded
containers to validate APIs. Host package databases were not modified.

## Remaining integration boundaries

Parent must merge this source with GPU commits, run final combined full CI/native
build, and capture the real UI. Binary backend availability at that stage
must remain accurately advertised; Kraken-only test builds do not become universal
receiver builds. Physical SDR vendor acceptance, actual native installation,
serial/wiring/coherence and SDRplay license acceptance are not claimed by offline
tests. No ADS-B inference inputs or bearing eligibility changes were introduced.
