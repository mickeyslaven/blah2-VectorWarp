# Maintainer release guide

VectorWarp publishes signed Linux packages, a notarized macOS package, a
Homebrew formula, and the package download page through GitHub Actions.

## One-time setup

1. Enable GitHub Actions and select **GitHub Actions** as the Pages source.
2. Protect the `release-signing`, `github-pages`, and `homebrew-publish`
   environments.
3. Add the package signing subkey as `VECTORWARP_ARCHIVE_SIGNING_KEY` and its
   40-character public fingerprint as
   `VECTORWARP_ARCHIVE_KEY_FINGERPRINT`. Commit only the public key at
   `packaging/keys/vectorwarp.asc`.
4. Add the Homebrew tap deploy key as `HOMEBREW_TAP_DEPLOY_KEY` in the
   `homebrew-publish` environment. Give it write access only to
   `mickeyslaven/homebrew-vectorwarp`.
5. On the signing Mac, install the release agent:

   ```sh
   python3 script/install-macos-release-agent.py --check
   python3 script/install-macos-release-agent.py
   ```

## Validate a change

Pull requests run the CPU, API, browser, macOS, receiver, package, and bundled
Kraken workflows. `release-packages.yml` builds the same native package profile
used by releases without publishing it or receiving signing credentials.

To repeat an installed-package check on a disposable Linux host:

```sh
npm ci --prefix test/browser
test/browser/node_modules/.bin/playwright install --with-deps chromium --only-shell
sudo env "PATH=$PATH" bash script/test-installed-package.sh \
  --package /absolute/path/to/vectorwarp.deb \
  --image docker.io/library/ubuntu:22.04 \
  --browser-modules test/browser/node_modules --evidence service-evidence
```

Use the base image matching the package being tested.

## Publish a release

1. Merge passing changes to `main`, freeze that exact commit, and complete the
   final artifact qualification and live soak below. Only then create an
   immutable `vMAJOR.MINOR.PATCH` tag at the qualified commit.
2. `release-packages.yml` builds and checks the DEB and RPM matrix. Signing and
   draft publication require an explicit dispatch with `publish` enabled at
   the qualified release tag; an ordinary merge or tag does not authorize it.
3. The macOS release agent verifies the exact tag, signs and notarizes the Mac
   package, attaches its source and receipt, and publishes the complete draft.
4. `publish-package-repository.yml` verifies the release, signs APT/DNF
   metadata, and deploys the download page through Pages.
5. Verify the public install page, package matrix, checksums, signing key, and
   clean-host installation.

The package matrix covers supported Ubuntu, Debian, Fedora, x86-64, and ARM64
targets. DragonOS uses its matching Ubuntu repository. Raspberry Pi OS uses the
matching Debian ARM64 package. The RSPduo package contains the local adapter
source kit; users install SDRplay's API separately and build support in Settings.

## Publish Homebrew

Before publication, satisfy the release acceptance conditions below for the
candidate being advertised. A prior version's soak is not current evidence.

`homebrew.yml` builds and tests on merges to `main`; those runs cannot publish.
After acceptance, an explicit dispatch with `publish` enabled regenerates the
formulas, verifies the exact current `main` source, and fast-forwards the public
tap. It does not require a Linux release tag.

For a local formula build:

```sh
script/package-homebrew-local.sh build/homebrew-local --install-tap
brew install --build-from-source vectorwarp/local/vectorwarp
```

## Final candidate and 24-hour live acceptance

Complete source fixes, UI cache changes, config migrations, packaging fixes and
release-documentation changes before freezing the candidate. Native builds
record `source_clean`; packaging rejects dirty or unproven artifacts and dirty
packaging checkouts. Keep generated evidence outside the source checkout or
under an ignored build directory. Build and test the final merged commit;
pre-merge artifacts do not identify that commit. Any subsequent source or
artifact change requires new qualification and a fresh soak cohort.

Preserve the exact downloadable package SHA-256, build manifest, build logs,
dependency notices/source closure and installation/config-preservation receipts.
Retain the deployment record connecting that package or native artifact to the
installed tree. The installed-tree digest below also covers API/UI assets,
modules, file modes and internal symlink targets; it is a different digest from
the compressed package SHA-256. Do not infer package qualification merely from
a matching source commit when release jobs rebuild the binaries.

For a publishable native-package candidate, explicitly dispatch **Build release
packages** from `main` with the intended `version` and `publish=false`. Preserve
the successful run ID and attempt, then deploy its exact `unsigned-package-*`
artifacts. Soak the package-installed tree, not the pre-packaging native build:
packaging changes integration files, and any deployment-local tree mutation
invalidates that package's qualification. Candidate artifacts are retained for
30 days; expire or lose them and a new candidate/soak is required.

For the Linux homeportal/HP cohort, run the read-only identity probe from this
frozen checkout on each deployed node (with permission to read the running
units' `/proc` metadata):

```sh
python3 tools/vectorwarp_soak_identity.py
ssh -o BatchMode=yes operator@hp python3 - < tools/vectorwarp_soak_identity.py
```

The probe assumes `/opt/vectorwarp/current`, `/etc/vectorwarp/config.yml` and
the standard API/processor systemd units. It verifies that both running units
use that config and artifact. Both must already have been started from the
final candidate and final config through the reviewed lifecycle. Hashing is
bounded to 30,000 entries and 2 GiB; special files, broken/external symlinks,
unreadable or concurrently changed files fail closed. Recordings and mutable
state must live outside the immutable release tree.

Review the probe outputs against the deployment record, then save a binding
JSON outside the checkout with this shape (all digests are complete lowercase
SHA-256 values, and `sourceCommit` is the complete 40-character commit):

```json
{
  "schema": 1,
  "sourceCommit": "<frozen commit>",
  "candidateRunId": "<successful explicit main package run ID>",
  "candidateRunAttempt": 1,
  "nodes": {
    "homeportal": {
      "packageFilename": "<exact downloaded package basename>",
      "packageSha256": "<SHA-256 of that downloaded package>",
      "release": "<resolved release directory>",
      "artifactSha256": "<artifactSha256 from reviewed probe>",
      "configSha256": "<configSha256 from reviewed probe>",
      "runtimeExecutables": "<copy the probe's runtimeExecutables object>"
    },
    "hp": "<same fields from the HP probe>"
  }
}
```

Set `VECTORWARP_SOAK_BINDING` to that file,
`VECTORWARP_SOAK_HP_SSH` to the authenticated SSH destination, and
`VECTORWARP_SOAK_STATE` to a new evidence directory. Run
`tools/vectorwarp_two_node_soak.py` every minute on homeportal. The monitor
measures both trees, configs and running executables on every observation and
binds the monitor/probe hashes into the cohort. Preserve the binding, state
and deployment receipts. Changing a binding or verifier requires a new state
directory; do not reset or overwrite failed evidence.

Release acceptance requires `releaseAccepted: true`: valid binding, no sticky
startup/capture/restart/binding failure, and at least 86,400 continuous healthy
seconds with no observation gap over 180 seconds. `latestHealthy`, a short
diagnostic window, or unbound `steadyStateAccepted` cannot release a candidate.
The monitor explicitly requires recording to be disabled; recording stress
and recovery tests are separate acceptance evidence.

Only after acceptance, dispatch **Build release packages** from the immutable
version tag with `publish=true`, `candidate_run_id` and `soak_receipt`. The latter
is a JSON object containing `bindingJson` (the exact binding file contents as
a JSON string, including whitespace) and `summary` (the final summary object).
The protected `release-signing` approver must review and attest the host
evidence; unsigned host JSON is not independent remote attestation. Promotion
authenticates the prior successful same-source/main workflow run, downloads its
existing packages without rebuilding, and compares actual package SHA-256,
installed-tree and runtime executable hashes to the accepted binding. It also
checks the frozen monitor/probe hashes and the full 24-hour policy. Signing must
preserve all payload hashes; only RPM signature/container bytes may change.
The signed `release-soak-provenance.json` records this unsigned-to-signed chain.

The same source must pass the native package install/upgrade/remove matrix,
macOS source/standalone checks, and physical GPU qualification in
[GPU_ACCELERATION.md](GPU_ACCELERATION.md). Linux FP32-only and FP64 parity
receipts must cover the same unified artifact. A two-node soak does not qualify
every receiver or OS. Physical SDR checks and any separately rebuilt package
still need their own evidence. macOS FP64 residency is unsupported; Pi preview
fresh-card boot/Wi-Fi acceptance remains pending as documented in
[PI4_GUIDE.md](PI4_GUIDE.md). Do not promote those limitations into tested claims.
Signing/notarization credentials, trusted GPU runners and final publication
authorization are external release prerequisites.

## Operations

- Inspect the macOS release agent with
  `launchctl print gui/$(id -u)/com.vectorwarp.macos-release-agent`.
- APT metadata is renewed by the scheduled package-repository workflow.
- Rotate the signing subkey and Homebrew deploy key before they expire, keeping
  an overlap period so existing installations can receive the new public key.
