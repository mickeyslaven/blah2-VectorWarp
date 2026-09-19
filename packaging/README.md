# Native release packaging

For macOS, use the [local Homebrew packaging guide](../docs/MACOS_HOMEBREW.md).
The Mac app and Kraken companion formulas live in `Formula/`;
[automatic public tap updates](../docs/HOMEBREW_PUBLISHING.md) are prepared for
merges to `main`, with first publication still pending. No bottles are supplied.
The DEB/RPM instructions below apply to Linux.

To install VectorWarp, use the [downloads and APT/DNF page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
This document covers building and maintaining the packages.

The previous Raspberry Pi OS Trixie installation route is deprecated for new
Pi deployments. Its replacement is a planned **Pi 4 Bookworm 64-bit Lite image**;
**Pi 5 remains future work**. Add and validate the matching Bookworm ARM64
package target before building that image. No flashable image is published yet.
See [Pi image packaging](../docs/PI_IMAGE_PACKAGING.md).

Packages are published through the signed APT/DNF repositories and direct
release assets. This directory describes the release build for
Ubuntu 22.04/24.04/26.04, Debian 12 Bookworm ARM64, Debian 13, and Fedora 44.
Ubuntu, Debian 13, and Fedora 44 cover x86-64 (amd64 / x86_64) and ARM64
(arm64 / aarch64); Debian 12 is ARM64 only. Ubuntu/Debian produce DEBs; Fedora
produces RPMs. DragonOS selects a matching Ubuntu repository only when its OS
metadata matches. Raspberry Pi OS Lite 64-bit Bookworm selects the Debian 12
ARM64 package; the older Trixie route remains available through Debian 13.

Each release package includes compiled Kraken, USRP and dual-HackRF support,
our locally buildable RSPduo adapter source kit, and all replay formats. UHD,
libhackrf, compiler tools and binutils are native package dependencies.
HackRF One USB access follows each distribution's native rule. Ubuntu/Debian
install the processor account into the existing `plugdev` group; Fedora's
packaged VectorWarp udev rule grants the `vectorwarp` group access without
changing the vendor mode or desktop-seat ACL. The web API account receives
neither grant. Start or restart the processor explicitly to load its new group
membership; with version 0.1.7 or newer, use the checked `vectorwarp restart`
action. On Fedora also reconnect a HackRF so udev applies the rule.
Administrators can mask it with an identically named rule in
`/etc/udev/rules.d`.
Users install SDRplay's licensed API and headers separately, then build our
adapter from Settings. No vendor files are packaged.
Kraken still needs Heimdall and its USB setup. Settings can check and reuse
receiver software, or offer supported setup actions.

Public PR checks and trusted release builds use the same unified package
profile without any proprietary SDK. The `compiled_receivers` and
`local_build_receivers` fields describe those capabilities separately; package
smoke tests check the adapter files and source-kit integrity. A passing package
build is not a physical RSPduo test. See the
[maintainer setup](../docs/MAINTAINER_RELEASE.md) and
[SDRplay user setup](../docs/SDRPLAY_SETUP.md).

Every DEB/RPM build also exercises the installed `vectorwarp` commands in a
disposable systemd environment: default/open, start, stop, restart, status,
logs, version and help. Start/restart must produce fresh replay frames; stop
must stop the web interface too. These tests use simulated IQ, not attached radios.

`package-native.sh` uses an already extracted, checksum-verified official Node
runtime from `node-runtime.env`; it never downloads one. It packages required
licence notices. Example, after building an artifact:

```sh
script/package-native.sh \
  --format deb \
  --distro ubuntu24.04 \
  --version 1.0.0 \
  --artifact build/native/artifact \
  --node-runtime /path/to/node-v24.21.0-linux-x64 \
  --output-dir dist
```

A fresh installation creates dedicated users and directories and starts the web API,
but never starts radar processing. Starting with version 0.1.7, the installed
`/usr/bin/vectorwarp` launcher opens the web page without starting radar, or
provides fixed VectorWarp service start, safe stop and ordered restart actions
plus status, logs, version and help. Stop includes VectorWarp's web API and
helper, but never separately managed receiver/vendor services. Upgrades to 0.1.7 or newer quiesce
only VectorWarp's own services before unpack and reactivate only those
previously running; stopped processing remains stopped. They do not rebuild a
stale locally compiled RSPduo adapter or restart vendor services.
`/etc/vectorwarp/config.yml` is preserved. A first install with the local
RSPduo kit may start an already-installed standard SDRplay API service; it
never downloads the API or accepts its license. See
[SDRplay setup](../docs/SDRPLAY_SETUP.md).
Repository publication requires a maintainer-controlled OpenPGP key
and GitHub Pages; neither a private key nor a live repository is in this tree.

For capability and validation detail, link to the existing
[upstream comparison](../docs/UPSTREAM_COMPARISON.md) and
[Pi validation](../docs/PI4_VALIDATION_20260910.md).
