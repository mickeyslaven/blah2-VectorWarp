# DragonOS package-selection notes

Use the [source installation guide](INSTALL.md) for now; signed VectorWarp
packages and its APT repository are not yet published. There is no separate
DragonOS build. The future release installer selects the matching Ubuntu
repository from `/etc/os-release`:

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
installation, or radio-operation validation on DragonOS. Planned release
packages contain all four receiver adapters; SDRplay's API and Kraken Suite
remain separate installations. Current source builds include the receivers
selected by `--backend`; see [build choices](INSTALL.md#2-choose-the-receivers-to-include).

When the signed repository is actually published, inspect its installer before
running it as root. Until then, the repository and packages remain unpublished.
The DragonOS project downloads are at
<https://sourceforge.net/projects/dragonos-focal/files/>.
