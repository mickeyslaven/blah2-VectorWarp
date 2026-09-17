# DragonOS package-selection notes

There is no separate DragonOS build. The installer selects the matching Ubuntu repository from
`/etc/os-release`:

- Jammy / Ubuntu 22.04
- Noble / Ubuntu 24.04
- Resolute / Ubuntu 26.04

## 1. Install VectorWarp

Download over HTTPS and inspect the script before granting privilege. This block
configures the matching repository, installs VectorWarp, and opens its web
interface without starting radar:

When the script opens in `less`, press **q** after reviewing it to continue.

```bash
curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && \
  less vectorwarp-install.sh && \
  sudo bash vectorwarp-install.sh --repo-only && \
  sudo apt update && sudo apt install vectorwarp && \
  vectorwarp
```

If needed first, install `curl` and `gnupg` from DragonOS's matching Ubuntu
repository. `--repo-only` adds repository configuration only. The optional
`--start-web` installer route enables and starts only the web API directly.
On upgrades, package hooks may restore previously running radar.

## 2. Configure and start radar

In **Settings**, configure the receiver, then choose **Save & Restart**. This
saves the first configuration and starts radar; no separate start command is
needed.

## 3. Use controls and help

With a version 0.1.7-or-newer package, `vectorwarp` opens the web page or
prints its address over SSH without starting radar. `vectorwarp start` starts
the full VectorWarp stack; `vectorwarp stop` stops it including the web page;
and `vectorwarp restart` restarts it in order. `vectorwarp status`,
`vectorwarp logs`, `vectorwarp version`, and `vectorwarp help` inspect it.
These commands do not stop separately installed Kraken Suite or SDRplay services. Older
packages do not contain this launcher.

## 4. Update

Keep the configured repository; do not rerun the installer:

```bash
sudo apt update && sudo apt install vectorwarp
```

DragonOS has independent edition labels, including FocalX, Noble, and Resolute.
Those labels alone are not trusted for package selection. The installer checks
the Ubuntu codename (`UBUNTU_CODENAME` or `VERSION_CODENAME`) and, only when no
codename is present, an inherited Ubuntu `VERSION_ID`. Conflicting or unsupported
metadata stops without changing the machine.

For example, DragonOS FocalX R37.1 x86-64 reports Ubuntu 22.04 (Jammy): it
needs the Jammy amd64 package, not an Ubuntu 26.04 package. Let
`sudo apt install ./matching.deb` resolve dependencies, replacing `matching.deb`
with the downloaded filename. Do not mix libraries from different Ubuntu releases.

OS selection has metadata-fixture tests, and matching Jammy dependencies resolve
in a clean Ubuntu 22.04 APT simulation. A DragonOS user also reported a successful
APT installation; this is not a full DragonOS ISO or hardware acceptance test.
Packages contain
compiled Kraken, USRP and dual-HackRF adapters plus a local
RSPduo source kit; SDRplay's API and Kraken Suite remain separate installations.
Current source builds include the receivers selected by `--backend`; see
[build choices](INSTALL.md#2-choose-the-receivers-to-include).

Inspect the repository installer before running it as root. Direct packages are
listed on the [download page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
The DragonOS project downloads are at
<https://sourceforge.net/projects/dragonos-focal/files/>.
