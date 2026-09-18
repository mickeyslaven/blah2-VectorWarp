# DragonOS package-selection notes

There is no separate DragonOS build. The installer selects the matching Ubuntu repository from
`/etc/os-release`:

- Jammy / Ubuntu 22.04
- Noble / Ubuntu 24.04
- Resolute / Ubuntu 26.04

## 1. Install VectorWarp

Copy this command. It installs the prerequisites, adds the matching signed
repository, installs VectorWarp, and opens the web interface without starting radar:

```bash
sudo apt update && sudo apt install -y curl gnupg && curl --fail --location --proto '=https' --tlsv1.2 https://mickeyslaven.github.io/blah2-VectorWarp/install.sh --output vectorwarp-install.sh && sudo bash vectorwarp-install.sh --repo-only && sudo apt update && sudo apt install -y vectorwarp && vectorwarp
```

Enter your administrator password if prompted. The
[installer source](../script/install-release.sh) is available to read separately.

## 2. Configure and start radar

Open **Settings**, configure the receiver, then choose **Save & Restart**. This
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

Direct packages are listed on the
[download page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
The DragonOS project downloads are at
<https://sourceforge.net/projects/dragonos-focal/files/>.
