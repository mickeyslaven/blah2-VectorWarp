# Native release packaging

Packages are published through the signed APT/DNF repositories and direct
release assets. This directory describes the release build for
Ubuntu 22.04/24.04/26.04, Debian 13, and Fedora 44 on x86-64 (amd64 / x86_64)
and ARM64 (arm64 / aarch64). Ubuntu/Debian produce DEBs; Fedora produces RPMs.
DragonOS selects a matching Ubuntu repository only when its OS metadata matches,
and Raspberry Pi OS Trixie selects Debian 13 ARM64. Neither is a separate image
or hardware validation.

Each release package includes compiled Kraken, USRP and dual-HackRF support,
our locally buildable RSPduo adapter source kit, and all replay formats. UHD,
libhackrf, compiler tools and binutils are native package dependencies.
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
upgrades. A first install with the local RSPduo kit may start an already-installed standard
SDRplay API service; it never downloads the API or accepts its license. See
[SDRplay setup](../docs/SDRPLAY_SETUP.md).
Repository publication requires a maintainer-controlled OpenPGP key
and GitHub Pages; neither a private key nor a live repository is in this tree.

For capability and validation detail, link to the existing
[upstream comparison](../docs/UPSTREAM_COMPARISON.md) and
[Pi validation](../docs/PI4_VALIDATION_20260910.md).
