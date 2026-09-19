# Receiver setup

For macOS, begin with [Homebrew installation](MACOS_HOMEBREW.md) and the
[Mac receiver guide](MACOS.md#runtime-scope). Local USB Kraken uses the
[Homebrew Heimdall companion](MACOS_KRAKEN.md), and receiver setup uses per-user
Mac actions. The systemd services, APT/DNF packages, and privileged enrollment
instructions below apply to Linux.

1. On Linux, install VectorWarp from the [package and APT/DNF page](https://mickeyslaven.github.io/blah2-VectorWarp/#install).
   On macOS, use the [Homebrew guide](MACOS_HOMEBREW.md). Then run `vectorwarp`
   to open **Settings** without starting radar.
2. Select and configure the receiver, then choose **Save & Restart** to save
   settings and start radar, including the first time. **Save for later** does
   not start it.
3. Later, `vectorwarp start` brings up the full stack using saved settings;
   `vectorwarp stop` stops it, including the web interface; and
   `vectorwarp restart` restarts it in order. These commands do not stop shared Kraken
   Suite or SDRplay services. `vectorwarp status`, `vectorwarp logs`, and
   `vectorwarp help` inspect it. Linux also has `vectorwarp version`; on macOS,
   use `brew list --versions vectorwarp`.

If you enabled a Mac Homebrew service, use `brew services stop vectorwarp` and
`brew services restart vectorwarp` for service control. A plain `vectorwarp stop`
leaves that supervisor running, so it restarts the child processes.

Use the
[source-build guide](INSTALL.md#build-from-source) for development or unsupported
systems. No Docker runtime is needed.

## Build from source

The [installation guide](INSTALL.md) is the single source for dependencies,
Node.js requirements, receiver build choices, GPU build packages and startup
commands. Choose the required `--backend` before building. Installing an SDR
driver later does not add an adapter that was omitted from the VectorWarp build.

## Check receiver software

Select your receiver, then choose **Check receiver software**. The result
separately reports the compiled adapter, its runtime dependencies and detected
software or devices. This check does not retune a receiver or start services.

If the page offers a local setup action, review it. Run the exact enrollment
command it displays in an administrator terminal on the radar host, return to
**Check receiver software**, and approve the proposed action there. Enrollment
does not install packages or start the service by itself. Remote Kraken setups
keep using their configured remote endpoint; local actions do not control it.

## KrakenSDR Suite V2

1. Install and configure [KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite)
   on the receiver host. Suite owns the USB devices and calibration; VectorWarp
   consumes its calibrated TCP stream.
2. Select **Kraken** in Settings. Enter the Suite host and IQ/control ports
   (normally 8091/8092). Use the receiver host's address for a remote Suite.
3. Match VectorWarp's sample rate to Suite's rate, choose the active channel
   count, and select dedicated or synthesized reference operation. Configure
   the reference and surveillance channels for the actual antenna connections.
4. Choose **Save & Restart**. Frequency, channel count and an explicitly chosen
   gain require a command acknowledgement and a later matching Suite status.
   A sample-rate mismatch blocks Apply: change it in Suite, then match it here.

Kraken supports 2–8 coherent input channels. Dedicated-reference mode excludes
that channel from surveillance. Array-reference mode synthesizes a common
reference from the selected channels; it is not bearing estimation.
Gain defaults to **Keep receiver setting**. An explicit `-1` selects Suite
automatic gain; 0–50 selects manual gain in dB.

For a local Suite service, **Check receiver software** can offer enrollment and
startup of an installed service. It does not install Kraken drivers. Set the
receiver/transmitter sites for geographic views and record the actual array
layout. Reported settings do not prove antenna order, calibration or RF reception.

## SDRplay RSPduo

Follow [SDRplay setup](SDRPLAY_SETUP.md) to install the vendor API and use the
local RSPduo source kit in the unified package or `--backend all` source build.
VectorWarp never downloads that API or accepts its license.

In Settings, choose the RSPduo profile, then frequency, output sample rate and
gain settings. With multiple RSPduos, enter the intended device's serial number.
Reference and surveillance gain reduction are separate values. Choose
**Save & Restart** to reuse a running API service or start a standard installed
service before capture. Startup failures appear in the interface.

The adapter applies both tuners' settings through SDRplay API 3.15 and checks
return codes. It does not independently read back RF tuning or prove coherence.

## USRP / B210

Use a build containing the USRP adapter and install UHD 4.1 or newer. Select
**USRP** in Settings; enter the intended device address, subdevice, antenna and
gain, then set frequency and sample rate. Choose **Save & Restart**. UHD opens
the receiver directly; there is no separate VectorWarp-managed UHD daemon.

Before streaming, the adapter checks both channels through UHD getters:
sample rate within 0.5 Hz, tuning within 1 Hz, gain within 0.05 dB, and exact
antenna/subdevice values. Mismatches stop startup. These are software readbacks,
not RF or clock-source verification; clock/time source is not a web setting.

The 6 MS/s replay path passes with clutter filtering, but this is not a sustained
B210 hardware test. The upstream B210 timeout/crash report still needs a physical
endurance test; defensive error handling is not evidence that it is resolved.

## Dual HackRF

1. Use two HackRF units with a shared clock and hardware trigger. Follow the
   [wiring notes and official hardware guide](../src/capture/hackrf/README.md).
2. Install libhackrf and use a VectorWarp build containing the HackRF adapter.
   **Check receiver software** can offer supported native package setup.
3. Run `hackrf_info` on the receiver host to identify both serial numbers.
   In Settings, put the reference device first and surveillance device second.
4. Set frequency, sample rate, LNA/VGA gain and amplifier selection, then choose
   **Save & Restart**.

The adapter checks both devices' open, setting and start return codes. It does
not read back frequency, gain or clock synchronization afterward. The 6 MS/s
replay check is not a physical USB-throughput or coherence test.

## Receiver settings: application and verification

**Save for later** stores pending settings without applying them or restarting.
**Save & Restart** applies settings and restarts processing; previously saved
changes use **Apply & Restart**. Check fresh processor status after the restart.
If startup fails, correct the reported issue in Settings before retrying.

Suite status/readback, SDK return codes and physical RF verification are
different checks. The receiver sections above explain each boundary; discovery
alone does not establish successful radar operation.

## Recording, replay and browser access

### ADS-B source

In **Settings → ADS-B planes**, discover a local readsb/dump1090 feed or enter
a remote ADS-B server address. Like upstream adsb2dd, a server address such as
`http://receiver/tar1090` is read at `http://receiver/tar1090/data/aircraft.json`;
enter the base address, not the JSON filename. A compatible dump1090/readsb
web endpoint works too. Local decoder-file discovery does not require tar1090.
Discovery does not install or start a decoder or scan
other hosts. The source is saved in `truth.adsb.tar1090`. Live ADS-B is disabled
during replay and preview; it is never radar detection or tracking input.

Using 3lips with several radar nodes? See [3lips setup](3LIPS_SETUP.md).
Stock 3lips uses an external adsb2dd service rather than this built-in converter.

### Recording replay

Portable `.blah2iq` files include channel-major complex-float32 IQ, sample rate,
frequency and channel count. Replay checks those values without opening hardware.
Legacy RSPduo, Kraken MCHQ, USRP float32 and HackRF signed-int8 files need their
matching format; legacy USRP also needs the original block length.

The browser/API defaults to port 3000 with no password. Use a trusted LAN/VPN
or an authenticated gateway; see [browser access](INSTALL.md#4-open-the-interface-and-configure-your-receiver).

Serve Settings from the API's own address. If hosting the UI separately, the
administrator must list its exact origin (scheme, hostname and port) in the
API service's `BLAH2_RECEIVER_ORIGINS` comma-separated setting. Custom config
clients must send that `Origin` and `X-VectorWarp-Intent: config-write-v1`, in
addition to the revision and receiver-sync headers. Recording control uses
`POST /capture/toggle` with `X-VectorWarp-Intent: recording-toggle-v1`;
GET requests do not change recording state.

## Evidence and limits

[Upstream comparison](UPSTREAM_COMPARISON.md) distinguishes implemented features,
automated tests, hardware evidence and planned work. See the
[fixed-range benchmark](GPU_BENCHMARK_20260911.md) for measured processing times.
