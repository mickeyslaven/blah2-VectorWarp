# Homebrew installation

The [public tap automation](HOMEBREW_PUBLISHING.md) is prepared for
`mickeyslaven/vectorwarp`. Its public formulas become installable after the
workflow is merged and its first Apple Silicon build and installation checks
pass. Until then, use the local source installation below. This distinction
keeps an empty tap from being mistaken for a published package.

## Public installation (after publication)

With Homebrew and Xcode Command Line Tools installed, add the public tap once:

```sh
brew tap mickeyslaven/vectorwarp
brew install vectorwarp
vectorwarp
```

The full formula name is `mickeyslaven/vectorwarp/vectorwarp`
(`owner/tap/package`). After adding the tap, the short package names work.
Homebrew builds the application and its Kraken companion from source; no
prebuilt bottles or `.pkg`/`.dmg` installers are currently published.

## Public-tap updates (after publication)

Once the public formulas have been published, update both components with:

```sh
brew update
brew upgrade vectorwarp vectorwarp-heimdall
vectorwarp restart
```

If VectorWarp runs as a per-user Homebrew service, use `brew services restart
vectorwarp` instead of `vectorwarp restart`. Both paths retain the user's
configuration and recordings. The public tap is empty until the first
main-merge publishing run succeeds; these commands do not apply to the local
development tap below.

## Local source installation

The current local installation does not require a published formula or bottle.
You need Homebrew, Xcode Command Line Tools, and a
checkout containing the macOS port. Run from that checkout's root. Homebrew
installs the formula's build and runtime dependencies; you do not need to run
the separate direct-source-build dependency commands first.

Generate the local formulas and install:

```sh
script/package-homebrew-local.sh build/homebrew-local --install-tap
brew trust vectorwarp/local
brew install --build-from-source vectorwarp/local/vectorwarp
brew test vectorwarp/local/vectorwarp-heimdall
brew test vectorwarp/local/vectorwarp
vectorwarp
```

The generated formula embeds a `file://` archive URL and SHA-256, so it is only
for this machine and source snapshot. `--install-tap` creates the temporary
`vectorwarp/local` tap in Homebrew's local tap directory without making a Git
commit or contacting a remote. It builds the open SDK configuration
(UHD/HackRF); SDRplay remains an explicit local SDK build and is never packaged.
It also installs a separate `vectorwarp-heimdall` companion for local USB
Kraken capture. Its driver stays private to that formula; see
[MACOS_KRAKEN.md](MACOS_KRAKEN.md) for source provenance and physical validation
limits. Remove it with `brew uninstall vectorwarp/local/vectorwarp-heimdall`
after removing VectorWarp, before untapping.
The Formula also builds the optional Vulkan backend from the pinned header-only
VkFFT 1.3.4 source resource. Its MIT notice is installed as `VkFFT-LICENSE`;
MoltenVK and the open Vulkan runtime are Homebrew dependencies. The processor
retains its CPU fallback when Vulkan is unavailable at runtime.

`vectorwarp` opens the web interface without starting radar. In Settings, choose
and configure a receiver or replay file, then select **Save & Restart**.
For a manually started instance, use the same command names on macOS:

```sh
vectorwarp                 # open Settings
vectorwarp start
vectorwarp stop
vectorwarp restart
vectorwarp status
vectorwarp logs
vectorwarp help
brew list --versions vectorwarp
```

`vectorwarp stop` stops its processor, local Kraken controller and web
interface. macOS does not currently provide `vectorwarp version`; use the
Homebrew command shown above. The tests check software integration without
opening a physical receiver.
Use Homebrew and `brew services` for Mac package and login-service management;
Linux `apt`, `dnf` and `systemctl` commands do not apply on macOS.

## Optional per-user service

Run `brew services run vectorwarp/local/vectorwarp` to launch a per-user
service without registering it at login. Run `brew services start
vectorwarp/local/vectorwarp` to install a per-user LaunchAgent. It runs
`vectorwarp supervise` in the foreground, monitors the owned API, processor and
active Kraken controller, and restarts the stack after a crash. `brew services stop
vectorwarp/local/vectorwarp` sends SIGTERM and removes only this user service.
These service commands use your saved processing configuration; they are not
required to open Settings for the first time.
When supervision is enabled, use `brew services stop vectorwarp` to keep the
instance stopped, or `brew services restart vectorwarp` to restart it. A plain
`vectorwarp stop` stops the child processes but leaves the supervisor running,
so it starts them again. `vectorwarp status` and `vectorwarp logs` still inspect
the managed instance.

## Update the local source installation

The local `vectorwarp/local` tap embeds a snapshot archive. It does not receive
public-tap updates: regenerate the snapshot after each source change, then
reinstall or upgrade both local formulas as shown below.

The application's companion executable and HTML links follow Homebrew's stable
`opt` path. Independent companion upgrades therefore take effect on the next
start; `brew test` checks that these links are not pinned to an older Cellar
version. Restart an active service after upgrading either component.

The local development formula has the deliberately fixed
`0.1.7-macos-dev` version. Regenerate its archive and use `brew reinstall
--build-from-source vectorwarp/local/vectorwarp-heimdall vectorwarp/local/vectorwarp`
after editing the source so both components use the new snapshot. To exercise the local
Homebrew upgrade path, use a monotonically increasing local revision:

The following shows an initial revision 1 install followed by revision 2. For
an existing installation, choose a revision greater than both installed
formulas (for example, greater than 17 on the development Mac). Use a different
output directory for each snapshot so an older formula's archive and checksum
remain consistent.

```sh
VECTORWARP_HOMEBREW_REVISION=1 script/package-homebrew-local.sh build/homebrew-r1 --install-tap
brew install --build-from-source vectorwarp/local/vectorwarp
VECTORWARP_HOMEBREW_REVISION=2 script/package-homebrew-local.sh build/homebrew-r2 --install-tap
brew upgrade --build-from-source vectorwarp/local/vectorwarp-heimdall vectorwarp/local/vectorwarp
```

On the Apple M2 development host, the app and companion were installed and
tested through local revision 17, including formula tests, synthetic replay,
lifecycle recovery, and the local Kraken pipeline. That is evidence for this
source snapshot and architecture only; it does not publish a Homebrew release.
The [Apple Silicon CI job](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739948)
also installed and tested both formulas, checked installed replay and the
synthetic Kraken pipeline, and removed the packages afterward.
The `macos-15-intel` source CI job covers CPU, open adapters, synthetic Kraken,
replay, API, browser and lifecycle checks, but no Intel Homebrew installation,
physical receiver or GPU result.

## Remove

Stop any registered service and the current instance, remove both formulas,
then remove the local tap:

```sh
brew services stop vectorwarp/local/vectorwarp
vectorwarp stop
brew uninstall vectorwarp/local/vectorwarp
brew uninstall vectorwarp/local/vectorwarp-heimdall
brew untap vectorwarp/local
```

## Configuration and validation

The service keeps its configuration, logs and recordings under the invoking
user's `~/Library/Application Support/VectorWarp`; Homebrew service environment
overrides may select a different state directory for an isolated test.

Homebrew relocates and signs libraries after compilation. The formula binds
the RSPduo source-kit manifest to the final installed core in `post_install`;
`brew test` checks that hash and loads the open receiver modules without
probing hardware. A changed core or manual SDK requires rebuilding the local
RSPduo adapter.

For an isolated installed service check, run
`python3 test/macos/homebrew_service_test.py` after installation. It creates a
private Homebrew configuration under a temporary `XDG_CONFIG_HOME`, runs
synthetic replay, verifies child-crash recovery and stops its transient service.
It refuses to run if a VectorWarp service is already loaded or registered at
login. The source test needs the documented Node dependencies available.
