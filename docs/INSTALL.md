# First install

Signed packages are planned but **not published**. When the first release is
announced, use this guide; until then use the [source build](SETUP.md).

Planned packages support Ubuntu 22.04/24.04/26.04, Debian 13, and Fedora 44 on
x86-64 and ARM64. DragonOS may use the matching Ubuntu package when its OS
metadata matches; it is not a separately validated DragonOS image. See
[DragonOS notes](DRAGONOS.md).

## Raspberry Pi

Choose the package for the installed 64-bit OS, not the board:

| Operating system | Planned package |
| --- | --- |
| Fedora 44 | Fedora 44 ARM64 RPM |
| Supported Ubuntu | Matching Ubuntu ARM64 DEB |
| Debian 13 or Raspberry Pi OS Trixie | Debian 13 ARM64 DEB |

No custom SD image is required. The installer does not change boot firmware or
Wi-Fi. Pi 4 replay/startup validation exists, but live real-time capture and
Raspberry Pi OS installation are not yet validated; see [Pi limits](PI4_VALIDATION_20260910.md).

## Install and open settings

After publication, inspect and run the bootstrap:

```bash
curl -fLO https://mickeyslaven.github.io/blah2-VectorWarp/install.sh
less install.sh
sudo bash install.sh --start-web
```

`--start-web` starts only the settings/API service. It does not start radar
processing or open a receiver. Open `http://HOST:3000/` from a trusted browser.

## Configure and receive

In **Settings**:

1. Select a receiver profile and enter its connection details.
2. Set frequency, sample rate, receiver/transmitter coordinates, and a writable
   recording directory.
3. For Kraken, match Heimdall host, ports, channel count, and reference mode.
   Set frequency and gain in Suite V2.
4. Correct validation errors, then choose **Save & Restart**. This starts the
   processor with the saved configuration.

Enable the processor at boot only when you want it to start automatically:

```bash
sudo systemctl enable vectorwarp-processor.service
```

Keep the UI on a trusted LAN/VPN: it has no login.

## Limits

- Published packages will support live Kraken/Heimdall input and replay. They
  do not install Kraken USB drivers or configure Suite V2.
- Live RSPduo, USRP, and dual-HackRF require the [source build](SETUP.md) and
  their vendor SDKs.
- Replay does not open radio hardware. macOS and Windows are browser clients,
  not processor hosts.
