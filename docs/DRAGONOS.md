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

DragonOS has independent edition labels. Its project download page lists Focal
X, Noble, and Resolute R1 editions; those labels alone are not trusted for
package selection. The installer checks the Ubuntu codename (`UBUNTU_CODENAME`
or `VERSION_CODENAME`) and, only when no codename is present, an inherited
Ubuntu `VERSION_ID`. Conflicting or unsupported metadata stops without changing
the machine.

The selector is tested with metadata fixtures only. No DragonOS ISO was
available for this work, so there is no claim of boot, driver, SDR, package
installation, or radio-operation validation on DragonOS. Packages contain
compiled Kraken, USRP and dual-HackRF adapters plus a local
RSPduo source kit; SDRplay's API and Kraken Suite remain separate installations.
Current source builds include the receivers selected by `--backend`; see
[build choices](INSTALL.md#2-choose-the-receivers-to-include).

Inspect the repository installer before running it as root. Direct packages are
listed on the [download page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
The DragonOS project downloads are at
<https://sourceforge.net/projects/dragonos-focal/files/>.
