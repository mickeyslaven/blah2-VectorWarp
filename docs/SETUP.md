# Set up VectorWarp

Install VectorWarp from the [package page](https://mickeyslaven.github.io/blah2-VectorWarp/#install)
or follow the [macOS Homebrew guide](MACOS_HOMEBREW.md). Run `vectorwarp` to
open the browser interface, then select **Settings**.

## Setup assistant

Select **Start setup**. VectorWarp looks for connected SDRs before opening the
first step. A single detected receiver is selected automatically. If several
receivers are detected, choose the one to use.

Each **Save step & Continue** button saves that step without starting or
restarting the radar. Completed steps can be opened again from the step list.

### 1. Receiver

Choose the receiver connected to the radar computer:

- **KrakenSDR Suite V2**
- **SDRplay RSPduo**
- **Ettus USRP / UHD**, including B210
- **Dual HackRF**

VectorWarp uses the detected connection automatically. Receiver-specific host,
port, serial, antenna, clock, gain, and channel controls remain available in
**Advanced → Receiver**.

If the selected receiver needs software, select **Open receiver software
setup** and use the action shown there. Return to the assistant when the
receiver reports ready.

### 2. Tuning

Enter:

- **Center frequency:** the broadcast frequency used as the illuminator.
- **Sample rate:** a rate supported by the selected receiver.

### 3. Sites

Enter the name, latitude, longitude, and altitude for:

- **Receiver site:** the antenna array location.
- **Illuminator site:** the transmitter location.

These positions are used for bistatic range, aircraft comparison, coverage,
and the map.

### 4. Radar

Set:

- **CPI:** the processing interval in seconds.
- **Maximum range:** the farthest aircraft range to search.
- **Maximum speed:** the fastest target to include.
- **Detect radar returns:** enables detections on the range–Doppler display.
- **Build persistent tracks:** joins detections across CPIs.

VectorWarp calculates the delay and Doppler search limits from the range,
speed, frequency, and sample rate.

### 5. Data

Choose whether to enable:

- **ADS-B comparison**
- **Automatic aircraft IQ clips**

The page shows estimated storage for an hour of manual recording and for each
automatic aircraft clip.

### 6. Review

Confirm that Receiver, Tuning, Sites, Radar, and Data show **Ready**, then
select **Finish & Start radar**. VectorWarp validates the complete setup,
applies it to the receiver, and starts processing.

## Receiver-specific setup

### KrakenSDR Suite V2

KrakenSDR Suite owns the radio, synchronization, and calibration. VectorWarp
connects to its IQ and control ports.

For a remote Suite, open **Advanced → Receiver** and enter the Suite host,
stream port, and control port. Match VectorWarp's sample rate and active channel
count to the Suite configuration.

Use the visual array editor to select the layout, assign reference and
surveillance roles, map channels to physical antennas, and enter the measured
antenna positions. Kraken supports coherent arrays with two through eight
active channels.

### SDRplay RSPduo

Select **SDRplay RSPduo**, then open receiver software setup if the SDRplay API
or VectorWarp adapter needs installation. The setup action checks the installed
API, builds the adapter, and reports when the receiver is ready.

Choose automatic gain, automatic gain per channel, or manual gain under
**Receiver**. The live header and Receiver settings show overload events for
the reference and surveillance tuners.

### Ettus USRP / B210

Select **Ettus USRP / UHD**. VectorWarp detects local UHD devices and fills the
connection when one receiver is found. Device address, subdevice, antenna,
clock, gain, and channel settings are under **Advanced → Receiver**.

### Dual HackRF

Connect two HackRF units with a shared clock and hardware trigger. Select
**Dual HackRF**, then assign the reference radio first and surveillance radio
second. Serial numbers, amplifier controls, and LNA/VGA gains are under
**Advanced → Receiver**.

## Array setup

Open **Settings → Receiver → Array** to configure coherent receivers. Start by
choosing the number of active antennas, then select a visual layout:

- Uniform linear array
- Circular array
- Cross or plus array
- Bearing array with a dedicated reference antenna
- Custom measured layout

Select an antenna in the diagram to change its channel, role, position, gain,
or orientation. The diagram compares the installed measurements with the
frequency-based reference spacing without stretching the array shape.

## ADS-B and MLAT

Open **Settings → ADS-B** to enable aircraft comparison and choose sources.
VectorWarp can use local readsb or dump1090 data, remote tar1090 servers, and
configured online exchange sources. Multiple sources are read in parallel and
the newest aircraft position is used.

For a remote tar1090 server, enter its base address, for example:

```text
http://receiver/tar1090
```

VectorWarp reads the server's aircraft data and keeps the winning source on
each aircraft record. The source status page shows update age, request time,
failures, contribution counts, and which source supplied each position.

## Recording

### Manual recording

Select **Start recording** on the Live radar page to capture raw IQ. Select it
again to stop. The recording appears on the **Recordings** page with its time,
length, size, receiver, channels, and associated aircraft.

### Automatic aircraft clips

Open **Settings → Recording → Automatic aircraft IQ clips**. Enable automatic
recording and set the pre-roll, post-roll, aircraft range, elevation, and
minimum time between clips. Eligible aircraft passes create labeled clips
without changing manual recording.

### Share a recording

On the **Recordings** page, expand a finished recording and select **Share
recording**. On the radar computer, VectorWarp opens the recording folder with
that recording selected. From another device, it downloads a portable package
containing the IQ, receiver and processing configuration, array geometry,
ADS-B timeline, and recording metadata.

## Replay

On the **Recordings** page, expand a recording and select **Replay recording**.
VectorWarp loads the configuration packaged with that recording and starts the
replay without opening a physical SDR.

Use the playback bar to pause, resume, scrub, rewind, or skip forward. Live
radar, maps, displays, and aircraft information follow the selected replay
time.

## Confirm the radar is running

After setup or replay starts, check:

1. The header shows **RADAR ONLINE**.
2. The Live radar frame continues to update.
3. **Settings → Receiver status** matches the saved receiver settings.
4. ADS-B shows **ONLINE** when aircraft comparison is enabled.
5. The **Recordings** page shows existing recordings and active captures.

## Commands

```text
vectorwarp          Open the browser interface
vectorwarp start    Start VectorWarp
vectorwarp stop     Stop VectorWarp
vectorwarp restart  Restart VectorWarp
vectorwarp status   Show service status
vectorwarp logs     Show service logs
vectorwarp help     List commands
```

For a Homebrew-managed macOS service, use `brew services start vectorwarp`,
`brew services stop vectorwarp`, or `brew services restart vectorwarp`.

For source installation, see [Install VectorWarp](INSTALL.md).

## Report a problem

Open **Settings**, select **Report an issue**, describe what happened in the
comments box, and create the diagnostic report. Attach that report to a
[GitHub issue](https://github.com/mickeyslaven/blah2-VectorWarp/issues).
