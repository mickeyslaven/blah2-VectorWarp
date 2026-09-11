# Native release packaging

Packages are **not published**. This directory describes the release build for
Ubuntu 22.04/24.04/26.04, Debian 13, and Fedora 44 on x86-64 (amd64 / x86_64)
and ARM64 (arm64 / aarch64). Ubuntu/Debian produce DEBs; Fedora produces RPMs.
DragonOS selects a matching Ubuntu repository only when its OS metadata matches,
and Raspberry Pi OS Trixie selects Debian 13 ARM64. Neither is a separate image
or hardware validation.

Published packages are Kraken-only: they support live Kraken/Heimdall input and
all replay formats. They do not install Kraken USB drivers or Heimdall. Live
RSPduo, USRP, and dual-HackRF require a source build with the applicable vendor
SDKs and licences; no vendor runtime is bundled.

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

Installation creates dedicated users and directories but never enables or
starts the API or processor. `/etc/vectorwarp/config.yml` is preserved across
upgrades. Repository publication requires a maintainer-controlled OpenPGP key
and GitHub Pages; neither a private key nor a live repository is in this tree.

For capability and validation detail, link to the existing
[upstream comparison](../docs/UPSTREAM_COMPARISON.md) and
[Pi validation](../docs/PI4_VALIDATION_20260910.md).
