# Hardware-agnostic read-only node federation

Any VectorWarp installation can be a portal/coordinator, a remote node, or
both. Nodes may use different supported receivers, channel counts, sample
rates, illuminators and optional truth sources. Federation transports already
processed products and their provenance; it does not assume a B210, Kraken,
RSPduo, particular hostname, site or network provider.

For ordinary local installations, open **Settings → Local radar nodes**, enter
the other VectorWarp computer's private IP address and API port, and select
**Add node**. The coordinator probes the node identity, versioned capabilities,
and status before saving it; this is explicit endpoint discovery, not a network
scan. Private IPv4, loopback, tailnet addresses and unscoped local IPv6 are
accepted. Public addresses, DNS names and scoped IPv6 addresses (such as
`fe80::1%en0`) are rejected by this form. For IPv6 use a ULA address, or use the
node's private IPv4 address. The browser's channel selector then lists every discovered
channel with its node name; System shows the combined local fleet.

The coordinator stores the discovered registry outside the application tree
with mode `0600`. `VECTORWARP_NODE_REGISTRY` may override that location for
packaging or managed installations. macOS uses
`~/Library/Application Support/VectorWarp/nodes.json`; ordinary Linux users use
`$XDG_CONFIG_HOME/vectorwarp/nodes.json` or `~/.config/vectorwarp/nodes.json`.
A Linux process running as root defaults to `/etc/vectorwarp/nodes.json`.
These paths and the federation protocol do not require a service manager.
The on-disk shape is:

```json
{"nodes":[{"id":"roof-node","label":"Roof node","expectedIdentity":"roof-node","url":"http://192.168.1.20:3000"}]}
```

The ID is a stable lowercase identifier, not an address. Set
`VECTORWARP_NODE_ID` and, optionally, `VECTORWARP_NODE_LABEL` on each API
service. Without an explicit ID, the API derives one from the computer's host
name; set distinct IDs on computers with identical names, and use explicit IDs
when the identity must survive a host-name change. Reserved IDs `local` and
`aggregate`, duplicate peer identities/URLs, and entries identifying the local
API are rejected. Existing registry aliases may differ from `expectedIdentity`;
the published identity and response checks still use `expectedIdentity`.
Every proxied response carries that identity; a missing or changed identity is
rejected. Node additions and removals made in Settings take effect immediately
and are written atomically. They also discard cached node products and reset
cooperative fusion state. Requests spanning a membership change return HTTP 409
and can be retried; they cannot republish removed-peer results. Re-adding a node
requires fresh discovery and reads. No addresses or credentials belong in
the public application tree.

A portal with no radar processor declares `VECTORWARP_NODE_ROLE=coordinator`
on its API service. The default role is `radar`. This explicit role is published
in status/capabilities and retained on discovery; only declared coordinators are
excluded from radar-node health. An unavailable radar remains degraded, even if
it has never published a frame. Managed registry entries may declare the same
`role` for existing coordinators. No host name or receiver type implies a role.

Finalized pass accounting retains a rolling 24-hour summary in the existing
statistics ledger while keeping only 500 detailed diagnostic rows. The compact
history has a 32-MiB per-ledger reserve; migration from older truncated ledgers
or exhaustion of that reserve marks the requested window incomplete until its
missing interval ages out. Lifetime totals remain independent of this window.
Federation preserves pass IDs, evaluation policies, timing quality and detector
run identities, including passes that span restarts. Its operational counts are
observations, not detector/tracker regression or release acceptance evidence.

The browser talks only to the coordinator API. The proxy accepts GET requests
for a fixed list of read endpoints, does not follow redirects, and limits
response size, duration and concurrency. Node and channel choice is per browser
tab; illuminator choices and caches are node-scoped. System reports the fleet
aggregate while retaining node provenance. Missing nodes and layers remain
visible as unavailable. Remote settings, capture, replay, sharing, import and
deletion are unavailable. Recording detail and package/file reads retain the
node ID in their URL; streaming is capped at 64 MiB per response, and bounded
byte ranges are supported where the upstream supports them. A full package
larger than this cap cannot be downloaded in one response.

Every current node exposes `GET /api/federation/capabilities`. Its versioned,
receiver-neutral resource list tells coordinators which processed products are
available. Aggregate views do not request unsupported layers. Older nodes that
pre-date this endpoint remain usable through conservative endpoint probing;
failure stays isolated to that node. Receiver type, channel count, sample rate
and center frequency are descriptive metadata, never feature switches.
