# Standalone macOS installer development

This guide describes the standalone macOS installer and its local verification.
Published packages, when available, are linked in the
[release download matrix](https://mickeyslaven.github.io/blah2-VectorWarp/#install).

The `.pkg` targets one `VectorWarp.app` in `/Applications`. The app
contains separate `arm64` and `x86_64` runtimes; its universal launcher
selects the matching runtime. macOS 15 or newer is the target. Homebrew is not
needed at runtime.

The local universal candidate has Developer ID Application and Installer
signatures. Its hardened-runtime checks passed on Apple Silicon, including CPU
lifecycle and actual Vulkan ambiguity/clutter execution. Apple notarization was
accepted and stapled; local Gatekeeper installer assessment passed. Actual installer
execution remains untested.
The ad-hoc mode described below remains available for development fixtures.

The standalone CI matrix passed both macOS 15 runtime jobs for source revision
`f8135ac947a88dd416b0f0ae8d3d891dd546da1d` ([run 35420421113](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35420421113)). This is runtime
CI evidence. The combined local app and installer also passed assembly, Apple
Silicon application lifecycle, unchanged-runtime and expanded-package checks.
The Intel payload was exercised in CI; physical Intel execution remains untested.
Release publication is verified separately through the public download matrix.

## Commands

The app opens Settings by default. Its executable is:

```sh
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp
```

Use that full path for lifecycle commands; the installer does not promise a
global command symlink:

```sh
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp start
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp stop
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp restart
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp status
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp logs
/Applications/VectorWarp.app/Contents/MacOS/VectorWarp help
```

State remains in `~/Library/Application Support/VectorWarp`, shared with the
Homebrew installation. Stop an existing Homebrew or standalone instance before
replacing or switching installations.
The installer refuses to replace a running app. Lifecycle commands also refuse
to overwrite the process records of another active installation. Stop it with
its original launcher (or `brew services stop vectorwarp` for a Homebrew
supervisor) before switching. Installing the package does not start processing
or create a login item, and it does not replace per-user settings or recordings.

## Included and separate components

The planned package targets CPU/replay operation and bundled open SDR modules.
GPU use remains optional and requires separate runtime acceptance. UHD device
image readiness is separate from the bundled UHD module.
GPU discovery alone does not enable acceleration: the processor also checks
accuracy and performance, and falls back to CPU with a reported reason when
qualification fails. The Intel CI virtual device fails forced-GPU replay
accuracy qualification and uses CPU; physical Intel GPU operation remains
unverified.

Kraken capture requires a separately built or imported local Heimdall companion;
the Suite is not bundled because its redistribution terms are not established.
SDRplay remains a manually installed vendor SDK with a local adapter build. Its
SDK and adapter are never bundled or downloaded by the installer.

For the available local source/Homebrew path, see
[MACOS_HOMEBREW.md](MACOS_HOMEBREW.md). Hardware, GPU, and platform limits remain
in [MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md).

## Building and qualifying an installer

`script/package-macos-standalone.py stage` takes a native artifact built by
`script/build-macos.sh`, explicit Node/Python executables and, for GPU builds,
the MoltenVK library. It copies and relocates their dynamic-library closure,
binds the optional source kit to the final signed core, and records file hashes.
An audit rejects external library paths, escaping symlinks, altered files,
wrong architectures and dependencies requiring a newer macOS version than the
declared minimum. The source checkout must be clean unless the explicit local
development option is used.

Build and test both architectures on their matching machines before combining
them. For two audited runtime directories from the same source revision:

```sh
python3 script/package-macos-standalone.py assemble \
  --arm64 /path/to/arm64-runtime \
  --x86_64 /path/to/x86_64-runtime \
  --output "$HOME/Library/Caches/VectorWarp/new-installer"
```

Use a fresh local output directory outside iCloud Drive or other synchronized
folders. Finder metadata prevented signing in the local synchronized Documents
checkout and remained after cleanup. The builder removes only the two metadata
attributes disallowed by [Apple signing rules](https://developer.apple.com/library/archive/qa/qa1940/_index.html)
from its generated app copy; it preserves quarantine and provenance attributes.

This creates `VectorWarp.app` and `VectorWarp-universal-local.pkg`. It does not
install, publish, or notarize them. A universal launcher selects one complete
native runtime; the package does not depend on Rosetta or an installed Homebrew.

The standalone CI matrix builds both runtimes on macOS 15, temporarily hides
Homebrew on the disposable runner, and tests replay, settings, SDK no-device
handling, lifecycle and the unchanged bundle inventory. Virtual-runner driver
discovery is separate from real GPU execution and physical receiver testing.
CI runs for pull requests, main-branch merges and version tags, uploading diagnostics and optionally
encrypted development payloads for local review. The recipient's private transfer
key and Apple signing/notarization credentials stay on the local Mac; this public
repository does not register that Mac as a self-hosted runner. Release packages
are assembled and signed locally with the reviewed corresponding source and
notices; an accepted, stapled PKG and release receipt are required before the
public download page links an asset. A new tag does not authorize reuse of a
prior release's source archive or notice bindings: changed dependency or runtime
inputs require fresh verification and may stop automatic publication.

For confidential CI review, `script/transfer-macos-runtime.py` can encrypt a
tested runtime to a public recipient certificate. The private key stays outside
the repository and CI. Restore requires explicit expected source, architecture
and run identifiers obtained from a trusted GitHub Actions run, and verifies
the decrypted runtime before publishing its local output directory. The CMS
envelope and its hash sidecar do not authenticate the sender: obtaining both
from an arbitrary location is insufficient. This development transfer does not
replace the source/notices distribution review.

The stage command records checkout identity and file hashes; the fresh
checkout/build/stage sequence in CI establishes that the native artifact came
from that source. When staging an artifact manually, the builder must preserve
the same source/build provenance. The diagnostic license inventory is not a
complete corresponding-source collector or a release approval check.

## Notices and final signing

Supply `assemble --notices-dir /path/to/reviewed-notices` when preparing a
distribution candidate. This directory contains the reviewed license texts and
a `notices.json` manifest with `schema: 1`, the runtimes' common `source_id`, both
runtime-manifest SHA-256 values in `runtime_manifest_sha256`, and a `files` map
from each notice's relative path to its SHA-256. The map excludes `notices.json`
itself. The builder rejects changed or unlisted files, symlinks, unsafe paths and
incorrect source/runtime bindings before creating the output. It places the
verified notices in `Contents/Resources/ThirdPartyNotices` before sealing the app.
Manifest integrity does not establish that every required notice is present.

Provide the reviewed corresponding-source archive beside a public installer,
with equivalent access, hashes and build/modify instructions. Original project
MIT notices remain; the native binaries also incorporate FFTW/UHD and other
dependencies with their own terms. The full binary's obligations are not described
by the root MIT license alone. Retain complete Node/Python notices, native-library
notices and sources, applicable patches, and the exact packaging scripts.

After source/notices review, use `script/sign-macos-standalone.py` to create a
fresh signing copy. It requires separate locally usable Developer ID Application
and Developer ID Installer identities and their public Team ID:

```sh
python3 script/sign-macos-standalone.py \
  --app "$HOME/Library/Caches/VectorWarp/new-installer/VectorWarp.app" \
  --output "$HOME/Library/Caches/VectorWarp/new-signed-installer" \
  --developer-id \
  --application-identity 'Developer ID Application: Your Name (TEAMID1234)' \
  --installer-identity 'Developer ID Installer: Your Name (TEAMID1234)' \
  --team-id TEAMID1234
```

The helper signs native libraries before their executables, applies only the
per-executable entitlement profiles, updates the local SDRplay adapter source
kit's signed-core binding and runtime/notice manifests, then seals the outer app.
It preserves original manifests and a signing-transformation receipt outside the
app. The source input remains unchanged. This produces `VectorWarp-signed.pkg`;
it does not submit to Apple, install the package or publish a release.

For local development, `--ad-hoc` replaces the Developer ID options and produces
`VectorWarp-local-ad-hoc.pkg`. This mode does not qualify hardened-runtime or
Developer ID behavior. Both modes refuse to replace existing output and require
a local output directory outside common synchronized folders.

Keep Apple credentials in Apple's local keychain tools. Notarize the final signed
package using a local `notarytool` profile, require an explicit `Accepted` result,
inspect its log, staple the package and verify it before public distribution.
Actual Developer ID startup, JIT and plugin/GPU loading passed on the local Apple
Silicon candidate; those results are separate from ad-hoc fixture tests. See the
[test matrix](MACOS_TEST_MATRIX.md) for scope and remaining hardware coverage.
Apple notarization, stapling and Gatekeeper installer assessment passed; actual
installation has not been tested.
