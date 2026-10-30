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

1. Merge passing changes to `main` and create an immutable
   `vMAJOR.MINOR.PATCH` tag.
2. `release-packages.yml` builds the DEB and RPM matrix, checks every package,
   and creates a draft release with checksums and manifests.
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

`homebrew.yml` runs after merges to `main`. It builds and tests both formulas on
Apple Silicon, regenerates them before publication, verifies that the source is
still current, and fast-forwards the public tap. It does not require a Linux
release tag.

For a local formula build:

```sh
script/package-homebrew-local.sh build/homebrew-local --install-tap
brew install --build-from-source vectorwarp/local/vectorwarp
```

## Operations

- Inspect the macOS release agent with
  `launchctl print gui/$(id -u)/com.vectorwarp.macos-release-agent`.
- APT metadata is renewed by the scheduled package-repository workflow.
- Rotate the signing subkey and Homebrew deploy key before they expire, keeping
  an overlap period so existing installations can receive the new public key.
