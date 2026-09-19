# Local USB KrakenSDR on macOS

The macOS port includes a native Heimdall companion and a private build of
KrakenRF's RTL-SDR driver. Standard KrakenSDR capture uses libusb; it does not
require Linux GPIO, systemd, a remote computer, or the Raspberry Pi DAQ stack.
The optional moRFeus downconverter and Kerberos Pi GPIO accessories remain
unsupported by this macOS companion.

## Homebrew installation

The local development tap installs `vectorwarp-heimdall` as a separate
dependency of `vectorwarp`. Generate both formulas with
`script/package-homebrew-local.sh build/homebrew-local --install-tap`, then
trust the generated tap with `brew trust vectorwarp/local`, then run
`brew install --build-from-source vectorwarp/local/vectorwarp`.
The companion driver lives inside its formula's private `libexec/lib/kraken`
directory. It does not replace Homebrew's stock `rtl-sdr` or install a global
receiver service. Neither formula includes SDRplay software.

For a direct source build, first build VectorWarp, then stage its companion
into the same artifact:

```sh
brew install cmake ninja pkgconf eigen fftw libusb
script/build-macos.sh --backend all --test --jobs 1
python3 script/build-kraken-macos.py --jobs 1 --output-dir build/macos/artifact
```

The explicit companion build fetches four commit-pinned source archives,
checks their SHA-256 values and applies the checked-in controller and driver
patches. It does
not enumerate receivers or start capture. Sources, compiler output and the
exact applied patch stay under `build/kraken-native/build-*` for diagnosis.
The artifact records source revisions and both patch hashes in
`share/heimdall/build.json`.

## Settings and lifecycle

Select **Kraken**, live capture, sample rate **2,400,000**, five channels for a
standard Kraken, and Heimdall host **127.0.0.1**. Factory serials 1000–1004
determine channel order. The default IQ/control ports are 8091/8092. This
managed Mac path accepts only the local controller; remote addresses are
rejected. Existing Linux endpoint behavior is unchanged.

The controller is started for live Kraken Apply/start/restart and is owned by
the same user instance as VectorWarp. Initial Apply uses a private candidate
configuration and waits for control acknowledgement/readback before replacing
the saved YAML. Controller failure is reported as a failed live start.
Web-only startup, replay and other receiver profiles do not open the Kraken.
Stop shuts down the processor, then its owned controller, then the API.

The controller binds its IQ, control, web and RTL-TCP listeners to 127.0.0.1.
Web/RTL ports default to 8070/1234; local operators can override these with
`VECTORWARP_MACOS_HEIMDALL_WEB_PORT` and
`VECTORWARP_MACOS_HEIMDALL_RTL_PORT`. All four ports must differ. A conflict
fails startup without taking over another process.

Receiver settings and logs live under the per-user VectorWarp state directory:
`kraken-controller/` and `logs/kraken.log`. Heimdall's installed HTML and binary
stay read-only. No browser tab is opened by controller startup. The companion
is a foreground child, not an independently enabled login service.

Readiness confirms the native controller and saved configuration match; it
does not claim calibration has finished. Pending/recovery status remains
visible, and the radar processor rejects uncalibrated, stale, noise-source or
retuning frames. Capture discontinuities must invalidate a radar CPI.

## Software verification and physical boundary

Run the native synthetic test against the same staged executable:

```sh
python3 test/macos/kraken_adapter_test.py
python3 test/macos/kraken/native_simulation_test.py \
  --heimdall build/macos/artifact/bin/heimdall \
  --include build/macos/artifact/share/heimdall/include \
  --output build/kraken-simulation
python3 test/macos/kraken_pipeline_test.py \
  --binary build/macos/artifact/bin/blah2 \
  --app-root build/macos/artifact \
  --heimdall build/macos/artifact/bin/heimdall \
  --include build/macos/artifact/share/heimdall/include \
  --output build/kraken-pipeline
```

The test rewrites the RTL-SDR dependency in a disposable binary to a fake
driver with no USB dependency. It exercises native capture, calibration,
known channel phase/gain errors, control changes and shutdown. It does not
qualify physical USB bandwidth, sample-clock drift, power stability, antenna
signals or real Kraken calibration. Hardware acceptance remains a separate
step after connecting the powered receiver. A separate physical run on the
Apple M2 development host passed internal-noise calibration, five-channel
2.4-MS/s capture, frequency/gain changes and clean restart/shutdown. The local
Homebrew app and companion were installed and tested through revision 17; see
[MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md) for the tested duration and limits.
External antenna/RF coherence and physical interruption recovery remain unqualified.
The [macOS CI run](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739948)
passed the source build and simulated native checks on Apple Silicon and Intel,
plus the installed Homebrew pipeline on ARM. CI does not establish physical
Intel receiver behavior; local runs only qualify the architecture actually used.

## Source provenance

`config/kraken-macos-sources.json` pins Suite commit
`a541354bc4fa02261cb521d98937842591e8ce27`, Kraken RTL-SDR commit
`08fb08165ecfcdd954c7a20cb1bbfbc159294f0e`, uWebSockets and uSockets.
The build installs dependency license files supplied upstream. The pinned
Suite repository has no repository-wide license file; its code is not
relicensed under VectorWarp's MIT license. These development formulas build
locally from upstream source; no companion bottle is published here.
