# DragonOS package-selection notes

There is no separate DragonOS build. Use the [package and APT setup page](https://mickeyslaven.github.io/blah2-VectorWarp/#install):
download and inspect the repository installer, then run it with `--start-web`.
This adds the matching Ubuntu repository, installs
VectorWarp, and starts only its browser interface; it does not start radar
processing. The installer selects the matching Ubuntu repository from
`/etc/os-release`:

- Jammy / Ubuntu 22.04
- Noble / Ubuntu 24.04
- Resolute / Ubuntu 26.04

With a version 0.1.7-or-newer package, run `vectorwarp` to open the web page
or print its address over SSH; that default action starts only the web API.
Configure the receiver there, then use `vectorwarp start` to bring up the
VectorWarp web API, helper and checked radar processor. `vectorwarp stop` safely
stops its processor, web API and helper; `vectorwarp restart` stops and starts
them in order. None of these commands stops separately installed Kraken Suite
or SDRplay services. `vectorwarp status` and
`vectorwarp logs` help inspect a failed start. Older packages do not include
this launcher; follow the versioned installation page for their controls.

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
