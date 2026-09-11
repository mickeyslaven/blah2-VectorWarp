# DragonOS package-selection notes

VectorWarp does not publish a separate DragonOS build. Its release installer
selects the matching VectorWarp Ubuntu APT repository from `/etc/os-release`:

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
installation, or radio-operation validation on DragonOS. The package remains
Kraken/Heimdall live-input capable and can replay all four recording formats.
For live SDRplay RSPduo, USRP or dual-HackRF, use the all-receiver source build
and the corresponding vendor software and host permissions.

When the signed repository is actually published, inspect its installer before
running it as root. Until then, the repository and packages remain unpublished.
The DragonOS project downloads are at
<https://sourceforge.net/projects/dragonos-focal/files/>.
