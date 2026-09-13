# SDRplay RSPduo setup

Install [VectorWarp](https://mickeyslaven.github.io/blah2-VectorWarp/#install) first,
then add SDRplay's API as described below.

The unified VectorWarp package includes our RSPduo adapter source kit, not
SDRplay's proprietary API or installer. You install the API, then VectorWarp
compiles the adapter locally from **Settings → Build SDRplay support**.
Source builders can use `--backend all` for the same workflow, or
`--backend rspduo` to compile the adapter before installation.
Obtain the compatible **SDRplay Hardware API 3.15** from
[SDRplay's official hardware API page](https://sdrplay.com/hardware-api/),
accept its terms yourself, and follow its installation instructions.
SDRconnect is a different product; installing it alone is not this API setup.

Install the API's development headers as well as its runtime. For the packaged
local builder, these belong in `/usr/local/include` and `/usr/local/lib` as
installed by the standard vendor installer. The package provides the normal
compiler tools; it never downloads SDRplay files or accepts their terms.

In VectorWarp, select RSPduo in **Settings**, then choose **Build SDRplay support**
when offered. The build checks the installed source kit, compiler and API;
progress or a specific error appears in Settings. Rebuild when the package or
SDK changes. Building does not start radar.

Then open **Check receiver software**:

- **Installed and running:** VectorWarp reuses the API without restarting it.
- **Installed but stopped:** select RSPduo and choose **Save & Restart**.
  VectorWarp starts a standard installed SDRplay service before processing.
  A first installation with the local RSPduo kit also attempts this;
  upgrades, staging and dry runs do not. It never enables boot startup.
- **Custom, ambiguous or overridden service:** automatic startup stops with an
  error. Have an administrator review and start it locally, then recheck in
  Settings. If **Check receiver software** offers a reviewed service action,
  follow the [local enrollment steps](SETUP.md#check-receiver-software).
- **Not found or not verified:** the page shows the official download link.
  If already installed, check its installation and service, then run the check again.

These are separate checks for the adapter, library and service. Finding a USB
device or SDK file does not prove that the receiver can capture coherent samples.
An unknown result means the check could not establish the state, not that the
software is definitely absent. No radio is opened during discovery.

Standard services `sdrplay.service`, `sdrplay_apiService.service` and the
locally enrolled `vectorwarp-sdrplay.service` alias are checked individually.
Custom non-systemd service arrangements may report an unknown service state.
Discovery never starts a service. Automatic startup respects local service-start
policy and requires a root-owned unit directly launching the installed native
`sdrplay_apiService` executable without extra commands or environment overrides.
If startup fails, the settings page stays available and reports the error;
VectorWarp does not start processing. Service activity alone does not prove RF capture.

Source builders also need the vendor's development headers and link library.
Use `BLAH2_SDRPLAY_INCLUDE_DIR` and `BLAH2_SDRPLAY_LIBRARY` for nonstandard
locations. These custom paths apply to source builds; the packaged local builder
uses only the standard paths above. Vendor software is not downloaded or licensed automatically.
