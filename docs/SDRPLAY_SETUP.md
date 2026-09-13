# SDRplay RSPduo setup

RSPduo-enabled VectorWarp builds include our adapter, not SDRplay's proprietary
API or installer. Source builds need `--backend rspduo` or `--backend all`;
the Kraken-only quickstart does not compile this adapter.
Obtain the compatible **SDRplay Hardware API 3.15** from
[SDRplay's official hardware API page](https://sdrplay.com/hardware-api/),
accept its terms yourself, and follow its installation instructions.
SDRconnect is a different product; installing it alone is not this API setup.

In VectorWarp, open **Settings → Check receiver software**:

- **Installed and running:** VectorWarp reuses the API without restarting it.
- **Installed but stopped:** start the vendor's service using its installation
  instructions, then run the check again. Administrators who have enrolled a
  local service can also use the separately approved **Start SDRplay** action;
  VectorWarp never starts it automatically.
- **Not found or not verified:** the page shows the official download link.
  If already installed, check its installation and service, then run the check again.

These are separate checks for the adapter, library and service. Finding a USB
device or SDK file does not prove that the receiver can capture coherent samples.
An unknown result means the check could not establish the state, not that the
software is definitely absent. No radio is opened during discovery.

Standard services `sdrplay.service`, `sdrplay_apiService.service` and the
locally enrolled `vectorwarp-sdrplay.service` alias are checked individually.
Custom non-systemd service arrangements may report an unknown service state.

Source builders also need the vendor's development headers and link library.
Use `BLAH2_SDRPLAY_INCLUDE_DIR` and `BLAH2_SDRPLAY_LIBRARY` for nonstandard
locations. End users installing VectorWarp packages do not need those build
headers. Vendor software is not downloaded or licensed automatically.
