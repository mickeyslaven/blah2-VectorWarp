# Standalone macOS installer development

This guide describes the planned standalone macOS installer. It is not a public
download or current release. Local implementation and testing are ongoing.

The intended `.pkg` installs one `VectorWarp.app` in `/Applications`. The app
will contain separate `arm64` and `x86_64` runtimes; its universal launcher
selects the matching runtime. macOS 15 or newer is the target. Homebrew is not
needed at runtime.

Local development artifacts are ad-hoc signed only. They must not be
treated as a public distribution until architecture CI qualification,
redistribution review, Developer ID signing, and notarization are complete.

## Planned commands

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
  --output /path/to/new-output-directory
```

This creates `VectorWarp.app` and `VectorWarp-universal-local.pkg`. It does not
install, publish, or notarize them. A universal launcher selects one complete
native runtime; the package does not depend on Rosetta or an installed Homebrew.

The standalone CI matrix builds both runtimes on macOS 15, temporarily hides
Homebrew on the disposable runner, and tests replay, settings, SDK no-device
handling, lifecycle and the unchanged bundle inventory. Virtual-runner driver
discovery is separate from real GPU execution and physical receiver testing.
Until the corresponding-source and notices review is complete, CI uploads
diagnostics and optionally encrypted development payloads for local review.
No public installer or automatic package-release workflow is enabled yet.

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
