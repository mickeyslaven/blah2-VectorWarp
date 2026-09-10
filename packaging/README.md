# Native release packaging

Release packages are built inside the operating system named by the asset:
Ubuntu 22.04, 24.04 and 26.04 produce Debian packages; Fedora 44 produces RPMs.
Both x86-64 and ARM64 are supported. An Ubuntu binary is never relabeled as
Fedora. DragonOS uses the matching Ubuntu repository selection at install time;
it is not a separately built DragonOS package or an ISO-tested claim.

Published packages use the Kraken-only compile mode. That mode supports live
Kraken/Heimdall input and replay of all four recording formats; it does not
install a Kraken USB driver or the separate Heimdall acquisition suite. The
full source build remains available for live SDRplay, UHD and HackRF support
when their external SDKs and licenses are satisfied.

The API uses an app-private official Node.js runtime pinned in
`node-runtime.env`. `package-native.sh` requires an already extracted,
checksum-verified runtime and includes its `LICENSE`; it never downloads one.
The artifact also carries the vcpkg dependency and VkFFT license notices. No
SDRplay, UHD, HackRF or other receiver-vendor runtime is bundled.

Example (after a native artifact has been built):

```sh
script/package-native.sh \
  --format deb \
  --distro ubuntu24.04 \
  --version 1.0.0 \
  --artifact build/native/artifact \
  --node-runtime /path/to/node-v24.21.0-linux-x64 \
  --output-dir dist
```

Package installation creates the dedicated users and writable directories but
never enables or starts the API or radar processor. `/etc/vectorwarp/config.yml`
is a Debian conffile and an RPM `%config(noreplace)` file, so local configuration
survives upgrades.

Repository publication additionally requires a maintainer-controlled OpenPGP
signing key in GitHub Actions secrets and GitHub Pages to be enabled. Neither a
private key nor a live repository is stored in this source tree.
