# First install

This is the package-first path for a supported Linux host. The signed package
repository is planned but **not published** until its first verified release.
Use its bootstrap only after that release is announced; until then, use the
[advanced source build](SETUP.md).

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
