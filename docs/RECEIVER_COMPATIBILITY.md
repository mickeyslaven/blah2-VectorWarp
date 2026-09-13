# Receiver software compatibility watcher

The scheduled workflow observes only the four sources pinned in
`config/receiver-software.json`: KrakenSDR Suite V2, UHD, libHackRF and the
SDRplay Hardware API for Linux. It never installs receiver software, changes a
service, accepts a vendor licence, flashes firmware, or publishes a release.

`lastSeen` records a reviewed source identity, not a compatibility claim. The
watcher accepts only HTTPS responses from the approved official hosts, bounds
responses, validates schemas and identities, and never treats a discovered URL
as executable input.

For a new UHD or libHackRF release, the hosted workflow may compile the pinned
source commit against one pinned VectorWarp commit in a temporary, credential-
free build job. This is source-level compile evidence only; it does not prove
hardware, firmware, coherent sampling, tuning, performance or packaging
compatibility. Kraken changes require manual protocol review. SDRplay remains
manual: automation neither downloads nor installs its licensed SDK.

The notification job is restricted to the canonical repository and default
branch. It receives only `issues: write`, validates bounded report artifacts,
and cannot create a PR, alter repository contents, publish a release, or access
release/signing environments.

To acknowledge a reviewed version, update only that catalog entry's `lastSeen`
identity and version in a normal reviewed change.
