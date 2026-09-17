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
Before building, VectorWarp checks the API 3.15 headers and the library's CPU
architecture. Missing or mismatched files produce a Settings error. The processor
also checks the version reported by the vendor API at startup; these checks do
not establish compatibility with a service running in a different OS environment.

In VectorWarp, select RSPduo in **Settings**, then choose **Build SDRplay support**
when offered. The build checks the installed source kit, compiler and API;
progress or a specific error appears in Settings. Rebuild when the package or
SDK changes. Building does not start radar.

An upgrade to VectorWarp 0.1.7 or newer restores only VectorWarp services that
were running before the upgrade; it does not rebuild this locally compiled
adapter or restart the separate SDRplay API service. If the installed core or
SDK changed, the old adapter may be marked stale and RSPduo processing will
refuse startup. Check **Build SDRplay support** in Settings, rebuild when
prompted, then use **Save & Restart**. An active processor service alone is
not proof of fresh RSPduo capture.

Set the receiver options, then choose **Save & Restart** to apply them.
**SDRplay startup** shows whether the SDK accepted the settings, the selected
serial number, and whether fresh radar frames have arrived. Expand its details
to compare the requested settings and completed SDK calls. This is not an
independent measurement of RF performance or phase coherence. Older adapters
without this report must be rebuilt to provide it.

LNA choices follow the tuned frequency: states 0–6 below 60 MHz, 0–9 from
60 MHz to below 1 GHz, and 0–8 from 1–2 GHz, for the two 50-ohm inputs used by
this adapter. State 0 provides the least gain reduction. Changing frequency
does not silently replace an existing LNA setting; choose a valid state before
saving. These limits match the published RSPduo table in section 5 of the
[SDRplay API 3.09 specification](https://www.sdrplay.com/docs/SDRplay_API_Specification_v3.09.pdf).
The current adapter has separately been tested with API 3.15 on a physical RSPduo.

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
