# First install

This is the package-first path for a supported Linux host. The signed package
repository is planned but **not published** until its first verified release.
Use its bootstrap only after that release is announced; until then, use the
[advanced source build](SETUP.md).

The planned APT targets are Ubuntu 22.04 (Jammy), 24.04 (Noble), 26.04
(Resolute), and Debian 13 (Trixie). Fedora 44 uses RPMs. All target
x86-64 (amd64 / x86_64) and ARM64 (arm64 / aarch64). The x86-64 package supports
both Intel and AMD CPUs; the aliases do not indicate different chip support.
Package filenames retain each distribution's native architecture name.
DragonOS may use the matching Ubuntu package when
its `/etc/os-release` metadata identifies one of those Ubuntu bases. It is not
an independently built DragonOS package and no DragonOS ISO boot/install has
been validated. See [DragonOS notes](DRAGONOS.md).

## Raspberry Pi

Choose the package for the installed operating system, not just the board:

| Operating system | VectorWarp package |
| --- | --- |
| Fedora 44, 64-bit | Fedora 44 ARM64 (arm64 / aarch64) RPM |
| Supported Ubuntu, 64-bit | Matching Ubuntu ARM64 (arm64 / aarch64) DEB |
| Debian 13 or Raspberry Pi OS Trixie, 64-bit | Debian 13 ARM64 (arm64 / aarch64) DEB |

Raspberry Pi OS Trixie is based on Debian 13. Its 32-bit edition is not a
supported package target. See the [official OS documentation](https://www.raspberrypi.com/documentation/computers/os.html).

No custom VectorWarp SD-card image is needed. The installer checks OS metadata
and package architecture; it does not configure the Pi's boot firmware or Wi-Fi.
Raspberry Pi OS installation, sustained radar throughput and Pi GPU acceleration
have not been hardware-tested. Hosted ARM builds do not establish those results.

## 1. Install and open settings

After the repository is live, download its bootstrap, inspect it, then install:

```bash
curl -fLO https://mickeyslaven.github.io/blah2-VectorWarp/install.sh
less install.sh
sudo bash install.sh --start-web
```

`--start-web` starts the settings/API service only. It does not start the radar
processor or open a receiver. From a trusted browser, open:

```text
http://HOST:3000/
```

## 2. Configure before receiving

In **Settings**:

1. Select the receiver profile and enter its connection details.
2. Set centre frequency, sample rate, receiver/transmitter coordinates and a
   writable recording directory.
3. For Kraken, make the Heimdall host, port, channel count and reference mode
   match Suite V2. Frequency and gain remain configured in Suite V2.
4. Use the displayed checks as configuration checks, then correct any field
   errors. They do not open the receiver or certify hardware health.
5. Choose **Save & Restart** to validate and apply the saved configuration.

**Save & Restart** deliberately starts the processor. To also start it after
future host reboots, optionally enable its service:

```bash
sudo systemctl enable vectorwarp-processor.service
```

Later settings changes use **Save & Restart**. Keep the UI on a trusted LAN/VPN:
it has no login. See [advanced setup](SETUP.md) for receiver permissions,
source builds, replay formats and troubleshooting.

## Notes

- The package targets the Kraken/Heimdall network receiver. It does not install
  Kraken USB drivers or configure a radio for you.
- RSPduo, USRP and dual-HackRF use the advanced source build and their vendor
  SDKs.
- Recording/replay works from paths accessible to the processor; replay does
  not open hardware.
- macOS and Windows are browser clients, not VectorWarp processor hosts.
