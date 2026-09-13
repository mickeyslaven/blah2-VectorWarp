# VectorWarp tests

VectorWarp retains the `blah2` binary and some target names for compatibility.
Tests are split by the component they exercise; they do not imply physical radio
or browser coverage beyond the stated fixtures.

## C++ and replay

From a configured build with testing enabled:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CTest includes unit coverage for processing, spectrum, tracking, detection math,
Kraken frame handling, recording formats and CPU/GPU fallback behavior when that
target is configured. Select one test with:

```bash
ctest --test-dir build -R testDetectionMath --output-on-failure
```

The offline full-processor replay harness exercises portable recordings for all
four receiver profiles through the built binary without receiver hardware.
Legacy-format readers have separate recording unit coverage.

```bash
python3 test/recording/processor_replay_test.py --binary bin/blah2
```

Use the actual configured output path if it differs from `bin/blah2`.

## API and browser fixtures

Node.js 22 or newer is required. Install the locked development dependencies;
the DOM test uses the pinned jsdom dependency.

```bash
cd api
npm ci --ignore-scripts --no-audit --no-fund
export NODE_PATH="$PWD/node_modules${NODE_PATH:+:$NODE_PATH}"
npm test
npm run test:receiver-management
npm run test:dom
cd ..
node test/ui/deployment.test.js
node test/ui/packaging.test.js
```

These checks cover configuration validation/recovery, processor status,
recording/replay controls, replay-only profiles, ADS-B projection, API/browser
behavior, native unit templates and package safety. They use temporary files,
loopback listeners and fixtures; they must not connect to a receiver or alter a
production service.

## Signed repository

Manifest validation needs only Python 3:

```bash
python3 test/packaging/test_repository.py
```

In an isolated Linux test runner with GnuPG, APT tools, RPM build/signing tools
and createrepo-c installed, also run:

```bash
VECTORWARP_REPOSITORY_INTEGRATION=1 python3 test/packaging/test_repository.py
```

Integration uses disposable format fixtures and a temporary test key. It checks
signatures, APT indexes, tamper rejection and unchanged RPM bytes during metadata
renewal; it does not install VectorWarp or publish a repository.

## Evidence boundary

Receiver-specific execution includes:

| Receiver | Native test | What it exercises |
| --- | --- | --- |
| Kraken | `krakenSocketCapture`, `testKrakenMultiChannel` | Actual capture over fragmented loopback TCP, reconnect/reset handling and invalid metadata; decoder, synthesis and fusion tests. |
| RSPduo | `rspduoStructuredFailures` | Actual adapter with SDK function stubs and real IQ buffers: settings, device selection, startup/cleanup failures, callback priming, pairing, gaps and counter wrap. |
| USRP | `usrpAppliedReadback` | Production setting/readback/receive-check helpers with a fake device; not the full UHD device/stream lifecycle. |
| HackRF | `hackrfAppliedSettings` | Actual adapter with SDK stubs: dual-device settings, callback routing and failure cleanup. |

RSPduo and HackRF tests are conditional on those adapters being enabled at
build time. RSPduo requires locally installed vendor headers; its fixture does
not link or run the SDRplay runtime. An omitted test is not a passing test.
Use `ctest --test-dir build -N` to check the actual inventory.

The receiver-management suite also runs the real API against loopback fixtures
for all four profiles. SDRplay install/start tests use simulated local services;
they cover missing/running/stopped/ambiguous services, local policy refusals,
upgrade exclusion and restart failures without touching the host.

Automated replay and fixture tests are not physical receiver, driver, live
capture, sustained GPU-performance or browser-rendering certification. See
[UPSTREAM_COMPARISON.md](../docs/UPSTREAM_COMPARISON.md) for the maintained
separation of implementation, automated tests and hardware evidence.
