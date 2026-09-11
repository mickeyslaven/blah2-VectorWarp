# Receiver setup implementation and acceptance — 2026-09-11

## Final source acceptance update

This update supersedes the historical unfinished-work paragraphs below. See
`SDR_MANAGEMENT_HANDOFF_20260910.md` for the current integration ledger.

Durable write-ahead receiver journaling and a packaged read-only processor-start
interlock are now implemented. Unknown receipts survive API restarts; pending,
unresolved and changed-revision configurations block live startup. Journal writes
preserve the shared config group for the separate processor account. Kraken
startup verifies fresh complete upstream status without issuing tuning commands;
existing native ingress additionally validates each IQ frame's channel count,
frequency and synchronization. These checks do not prove physical RF or wiring.

Local `enroll-service Kraken [UNIT]` / `enroll-service RspDuo [UNIT]` commands can
enroll fixed allowlisted existing services, including `krakensdr-suite-v2.service`,
`heimdall-v2.service` and vendor SDRplay service names. The operator reviews the
exact installed unit/executable/build hashes; no service is changed by enrollment.
Separate short-lived one-use local approval and browser execution remain required.
Root policy reload invalidates earlier grants; no arbitrary unit or command is
accepted from a browser. Remote existing Suite remains unmanaged and reusable.

`enroll-packages HackRF` / `enroll-packages Usrp` now create reviewed complete
add-only transactions for native Ubuntu/Debian APT or Fedora44 DNF5. An Ubuntu-like
DragonOS is accepted only if its package origins are authenticated native Ubuntu.
Every package/version/architecture/archive hash/origin and the installed-package
database revision are pinned. APT holds its frontend lock before opening the
cache through commit; DNF5 holds its system-repository write lock before loading
the RPM database through its transaction and fresh postconditions. Native signed
origins only; no upgrades/removals, arbitrary package roots, shell invocation,
new repositories, key import or automatic retry. UHD below4.8 is refused.

The browser displays the whole proposed transaction before execution. Missing
compiled backends show build guidance instead of an SDK action. Proprietary
SDRplay API acquisition/license acceptance remains a guided local vendor step.
Unsupported platforms or transactions return specific refusal instructions.
This is a qualified supported workflow, not a universal automatic installer.

Primary API references used during implementation:

- [python-apt cache/commit API](https://apt-team.pages.debian.net/python-apt/library/apt.cache.html)
- [APT SystemLock](https://apt-team.pages.debian.net/python-apt/library/apt_pkg.html)
- [DNF5 system-repository lock API](https://dnf5.readthedocs.io/en/latest/api/python/libdnf5_base.html)
- [DNF5 native transactions](https://dnf5.readthedocs.io/en/latest/tutorial/bindings/python3/transaction.html)
- [DNF5 signature inspection](https://dnf5.readthedocs.io/en/latest/api/python/libdnf5_rpm.html)

Final offline evidence adds 17 broker, 9 APT and 11 DNF injection tests; real
isolated API restart/journal tests; full API/DOM/packaging regressions; fresh
all-vendor CMake registration/build and both new receiver CTests. Real package API
smokes used Ubuntu22.04 python-apt2.4 and Fedora44 libdnf5/bindings5.4.4.0:
APT missing HackRF resolved three packages; DNF5 private-empty-RPMDB missing
HackRF resolved19 packages/10,076,597 bytes; installed Fedora UHD/HackRF reused.
No receiver/service action, actual package installation or hardware acceptance
was performed. Parent owns final combined build/CI and real UI screenshots.

## Historical initial review — 2026-09-10

This is a source change in the isolated management worktree, based on `ee275eb`.
No receiver, daemon, package manager, radio, GPU workload or research service was
started, restarted or installed. The shared experimental checkout was not edited.

## Implemented behavior

Settings → Receiver → **Receiver software** now checks all four backend profiles,
the installed build declaration, read-only USB descriptors, software presence and
the saved Suite endpoint. The browser distinguishes model detection from a
configured serial match. Two unrelated HackRF devices cannot satisfy the saved
pair; duplicate, absent or invalid configured serials fail correlation. A local
USB USRP does not stand in for a configured remote/unknown network target.
Existing available local or remote Suite endpoints are reused. A configured
remote Suite does not offer a local service-start action.

The independently reviewed Kraken transaction is integrated. A production live
Kraken Apply/Save & Restart always requires fresh full-state readback, including unchanged saves,
replay-to-live transitions and Save + Restart. Retuning and active-prefix changes
require the exact command ACK followed by a later coherent, non-reconfiguring
status with the requested complete tuple. YAML remains unchanged on command or
readback failure; receiver-applied/config-not-saved failures remain distinct.
The browser invalidates affected draft geometry assertions and reports unknown
transport outcomes. Channel serial ordering, wiring, calibration and actual IQ
are not inferred from status. Arrays remain configurable from two through eight
channels. ADS-B is not an inference input and bearing eligibility is unchanged.

First-run acceptance found and repaired an offline-save regression: the separate
**Save for later** button persists desired configuration even with missing Suite
software or missing first-run YAML. It uses explicit `mode=pending`,
`save-pending-v1`, trusted exact Origin and `restart=false`; it cannot fall back
from a failed Apply. It invokes no receiver synchronizer, helper or restart and
returns `saved-pending`, no operations, and no receiver-applied/verified claim.
An existing unresolved receiver outcome blocks this canonical pending save and
preserves its exact receipt. The API starts with application state `unknown`, not
verified; a fresh browser load still offers Apply for unchanged pending/unknown
settings. Pending persistence errors are explicitly unknown because directory
fsync can fail after YAML rename. This is not a processor-start interlock.

The HTTP management routes now include discovery, planning and execution. They
use trusted local numeric/loopback hosts or administrator-configured origins,
exact Origin for POST, JSON, an explicit intent header, a random server-held nonce,
the config revision, expiration and mutual exclusion with config/restart work.
No broad GET CORS applies. Read-only GET permits exact trusted Referer without
Fetch Metadata because browsers omit that header on ordinary HTTP LAN addresses;
a conflicting header is rejected. The browser normally uses intent-controlled
POST discovery.

The installed Python broker uses a root-owned Unix socket, Linux `SO_PEERCRED`,
the exact unprivileged API UID, an OS lease and one operation lock. The API cannot
authorize its own plan. The operator runs the displayed command in a local
administrator terminal, reviews the exact action, and grants it once. No Settings
password is added or stored. Both browser nonce and root-held grant expire, and
restart invalidates them. A mutation consumes its grant even if it fails. The
broker rechecks config revision, installed artifact and service state, then checks
the postcondition. API-writable config paths cannot redirect privileged reads
through symlinks, devices or FIFOs.

Executable actions are limited to reviewed `vectorwarp-heimdall.service` and
`vectorwarp-sdrplay.service` definitions. Root-owned policy pins the installed
build manifest, unit and executable hashes. Overrides, changed hashes and pending
systemd definition reloads block execution. Active means the fixed service is
active; it is not a claim about IQ, receiver compatibility or physical readiness.

Native staging includes the broker, socket/service, Python runtime dependency and
a preserved root-owned policy outside the API-writable config directory:
`/etc/vectorwarp-management/receivers.json`. Installation still starts no service.
Starting the API can activate the read-only management socket. Removing the
package stops its own broker/socket. No new sudo permission is granted to the API.

## Direct receiver repairs

RSPduo's 24 abrupt process exits are replaced by exceptions that reach processor
error telemetry. Startup validates first and tracks API lock, device selection
and stream initialization so cleanup is staged and idempotent. Both tuner
parameter records receive RF/AGC/IF/decimation/notch values while retaining
independent gains. Callback initialization now passes `this`; receiver removal
becomes a structured input failure. Capture also catches backend stop errors.

USRP reads rate, frequency, gain, antenna and subdevice mapping through the
[official UHD getters](https://files.ettus.com/manual/classuhd_1_1usrp_1_1multi__usrp.html)
before creating a receive stream. It rejects coercion or non-finite values, using
0.5 samples/s, 1 Hz and 0.05 dB numerical tolerances. These are SDK startup checks;
processor status still does not include an applied-values receipt. HackRF now
validates the two-element constructor inputs and frees descriptor inventories.
Its existing setters remain success-code checks; the
[vendor API](https://github.com/greatscottgadgets/hackrf/blob/main/host/libhackrf/src/hackrf.h)
does not establish independent RF readback for this path.

## Explicit unfinished work

The default privileged policy has **no approved actions**. This work does not
invent or take ownership of an arbitrary existing Suite/vendor unit. A reviewed
owned wrapper and corresponding root policy are required before its start button
can execute. Running configured Suite endpoints need neither ownership nor a
privileged action.

Automatic dependency installation remains **disabled**. Review found that pinned
top-level apt/dnf package names do not constrain all transitive dependencies or
concurrent package database changes. The helper can inspect exact reviewed
installed versions; missing packages produce `INSTALL_TRANSACTION_REVIEW_REQUIRED`.
There is no package-mutation command in the executor. Completion needs a full
signed transaction resolver, approval of every planned change, a package-manager
lock and postconditions. SDK installation also cannot add a backend omitted from
the processor: published packages remain Kraken-only, while selected/all-backend
source builds remain available. Four-backend release artifacts and vendor-aware
runtime packaging are separate unfinished acceptance work. SDRplay must be
obtained with the operator's vendor license acceptance; no redistribution or
automatic vendor installer is claimed.

No durable Kraken transaction journal, processor-start interlock or continuous
IQ/status agreement gate was added. No physical SDR, serial/cabling, calibration,
rendered-browser or final native-package-install acceptance is claimed. The
historical `RECEIVER_SYNC_INDEPENDENT_ACCEPTANCE.md` describes the imported sync
review; the RSPduo/UHD source repairs above happened after that review.

## Completed validation and next acceptance

The final full API suite, all nine independent Kraken transaction gates, 32
geometry acceptance cases, geometry/config DOM regressions, new management
browser flow and 15 Python broker injection tests passed on the disposable
`/tmp/vectorwarp-sdr-management-acceptance-final` snapshot in
`blah2-ui-config-preview`, using private
Node 24.21.0 and existing dependencies. Its bounds remained 0.5 CPU, CPUs 8/24,
8 GiB memory and equal swap ceiling. Native packaging acceptance passed; all nine
receiver build/staging tests completed with one CMake-only test skipped because
that UI environment lacks CMake. Shell syntax and `git diff --check` passed.

The three direct-backend sources passed C++17 compile-only checks against
already-installed vendor headers in the equally bounded `blah2-gpu-build`
container. An SDK-stubbed RSPduo fixture passed 15 startup/stream failure stages,
both-tuner assignments, callback instance delivery and idempotent cleanup. No
vendor runtime library was linked into the mock fixture. The SDK-independent UHD
mock accepted a matched tuple and refused 34 coercion/non-finite/channel/mapping
cases. The compile/fixture snapshot is
`/tmp/vectorwarp-sdr-compile-20260910` inside that container.

CI now invokes `npm run test:receiver-management --prefix api`. CTest registers
`usrpAppliedReadback` for ordinary test builds and `rspduoStructuredFailures` for
opted-in SDRplay source builds with locally supplied licensed headers. Both keep
assertions enabled in Release. Direct fixture compilations passed; full native
CMake configuration/build of these new registrations remains separate acceptance.

Follow-up full API, DOM, management and packaging regressions passed on the
`/tmp/vectorwarp-sdr-pending-20260911` UI-container snapshot. The new non-preview
API/DOM fixture covers missing first-run YAML, offline live Kraken endpoint/basic
edits, reload, zero receiver commands/restarts, strict pending intent, stale
revision, pre-rename and post-rename persistence failure, and unchanged Apply
requiring fresh receiver observation after API restart. Existing sync fixtures
also prove that pending saves preserve uncertain-command and receiver-applied/
config-persistence-failure receipts. The pending fixture runs in `test:dom` / CI.

The next acceptance is independent review of this complete diff and native CTest
registration, then the explicitly unfinished policy/packaging and physical-radio
work above. Both bounded test containers were released to the parent. No commit,
push, deployment, service action or promotion is authorized by this report.
