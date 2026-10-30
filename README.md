<p><img src="html/favicon/vectorwarp-vw.svg" width="64" height="64" alt="VectorWarp"></p>

# VectorWarp

> **THIS IS A DEVELOPMENT BUILD. EXPECT BUGS AND REPORT VIA GITHUB ISSUES PLEASE AND THANK YOU!**

VectorWarp is an open-source passive-radar application for Linux and macOS. It
supports KrakenSDR, Ettus USRP/UHD, HackRF, and SDRplay receivers. Its browser
interface provides live range–Doppler maps, radar tracking, bearing estimates,
recording, replay, receiver setup, and a 3D airspace map.

[Install VectorWarp](https://mickeyslaven.github.io/blah2-VectorWarp/#install) ·
[Setup](docs/SETUP.md) ·
[GPU acceleration](docs/GPU_ACCELERATION.md) ·
[Report a bug](https://github.com/mickeyslaven/blah2-VectorWarp/issues)

## See VectorWarp in action

### ADS-B-tracked aircraft

See ADS-B-tracked aircraft alongside radar detections and tracks on the live
bistatic range–Doppler display.

![VectorWarp live bistatic range-Doppler display](docs/media/vectorwarp-bistatic-range-doppler.gif)

### 3D live airspace

Explore terrain, altitude-separated aircraft, flight histories, radar tracks,
coverage, transmitter sites, and receiver sites on the same map.

![VectorWarp 3D live airspace map](docs/media/vectorwarp-3d-live-airspace.gif)

## Features

### Radar processing

- Live bistatic range–Doppler processing with configurable search bounds.
- GPU-accelerated clutter removal, detection, weak-target recovery, and
  tracking, with automatic CPU fallback.
- Persistent multi-CPI tracking with candidate, active, and coasting states.
- Automatic clutter suppression plus a learned stationary-clutter map.
- Coherent bearing estimation, calibrated beam banks, measured array
  manifolds, and bistatic geolocation tools.
- Automatic processing defaults with manual controls for tuning.

### Arrays and receivers

- Coherent 2–8 channel array support.
- Visual array designer for circular, linear, cross, dedicated-reference, and
  custom layouts.
- Clickable antenna roles, channel mapping, measured positions, and ideal
  frequency-based spacing in one view.
- Automatic gain or automatic gain per channel, with the selected values shown
  in Settings.
- KrakenSDR Suite V2, Ettus USRP/UHD including B210, dual HackRF, and SDRplay
  RSPduo support.
- Receiver discovery and guided setup that saves each step as it is completed.

### Live aircraft and maps

- Fused local and online ADS-B and MLAT feeds with source provenance.
- Remote ID integration for nearby drones.
- Interactive 3D aircraft, transmitter, and receiver models with true heading,
  climb, descent, altitude, and terrain.
- Altitude-colored flight trails, aircraft imagery, compass, coverage history,
  and selectable radar-track overlays.
- Map controls for all aircraft, radar-associated aircraft, or a clean radar-only
  view.
- Mobile touch controls for pinch zoom, rotation, and fullscreen.

### Record, review, and replay

- Manual IQ recording and automatic aircraft-triggered clips with pre-roll and
  post-roll.
- Portable recording bundles containing IQ, aircraft data, receiver settings,
  array geometry, timing, and processing configuration.
- Hardware-independent replay: a recording can be reviewed on a different SDR
  host by loading its packaged configuration.
- Browser playback with a floating timeline for pause, scrub, rewind, and skip.
- Download a complete recording bundle or reveal it in the local file browser.
- Recording labels, retention controls, storage status, and the metadata needed
  for replay.

### Browser control and diagnostics

- A responsive setup assistant and organized Simple, Advanced, and Experimental
  settings.
- Live health, timing, source, calibration, GPU, recording, and receiver status.
- Settings validation, automatic selections with their resolved values, and
  controlled Save & Restart.
- Sanitized issue reports with free-form comments, configuration, service
  status, and logs.

## Install

### Linux

Install the signed DEB or RPM for Ubuntu, Debian, Fedora, DragonOS, x86-64, or
ARM64 from the [VectorWarp package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).

```bash
# Ubuntu or Debian
sudo apt update && sudo apt install -y curl gnupg
curl --fail --location --proto '=https' --tlsv1.2 \
  https://mickeyslaven.github.io/blah2-VectorWarp/install.sh \
  --output vectorwarp-install.sh
sudo bash vectorwarp-install.sh --repo-only
sudo apt update && sudo apt install -y vectorwarp
vectorwarp
```

```bash
# Fedora
sudo dnf install -y curl gnupg2
curl --fail --location --proto '=https' --tlsv1.2 \
  https://mickeyslaven.github.io/blah2-VectorWarp/install.sh \
  --output vectorwarp-install.sh
sudo bash vectorwarp-install.sh --repo-only
sudo dnf install -y vectorwarp
vectorwarp
```

### macOS

```sh
brew tap mickeyslaven/vectorwarp
brew trust mickeyslaven/vectorwarp
brew install vectorwarp
vectorwarp
```

The browser opens directly into setup, discovers connected receivers, and
guides the first working configuration. SDRplay support uses its vendor API and
the setup action provided in Settings.

For source builds and additional installation options, see the
[installation guide](docs/INSTALL.md) and [macOS guide](docs/MACOS_HOMEBREW.md).

## Learn more

- [Receiver, recording, and replay setup](docs/SETUP.md)
- [GPU acceleration](docs/GPU_ACCELERATION.md)
- [Bearing and bistatic geolocation](docs/BEARING_GEOLOCATION.md)
- [3lips multi-node setup](docs/3LIPS_SETUP.md)
- [Changes from blah2](docs/UPSTREAM_COMPARISON.md)

## Credits and license

VectorWarp builds on the original [blah2](https://github.com/30hours/blah2)
radar engine. Licensed under the [MIT License](LICENSE).
