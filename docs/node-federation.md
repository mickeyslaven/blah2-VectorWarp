# Hardware-agnostic read-only node federation

Any VectorWarp installation can be a portal/coordinator, a remote node, or
both. Nodes may use different supported receivers, channel counts, sample
rates, illuminators and optional truth sources. Federation transports already
processed products and their provenance; it does not assume a B210, Kraken,
RSPduo, particular hostname, site or network provider.

For ordinary local installations, open **Settings → Local radar nodes**, enter
the other VectorWarp computer's private IP address and API port, and select
**Add node**. The coordinator probes the node identity, receiver capabilities,
and RF-channel catalog before saving it. Public addresses and DNS names are
rejected. The browser's existing channel selector then lists every discovered
channel with its node name; System shows the combined local fleet.

The coordinator stores the discovered registry outside the application tree
with mode `0600`. `VECTORWARP_NODE_REGISTRY` may override that location for
packaging or managed installations. The on-disk shape is:

```json
{"nodes":[{"id":"roof-node","label":"Roof node","expectedIdentity":"roof-node","url":"http://192.168.1.20:3000"}]}
```

The ID is a stable lowercase identifier, not an address. Set
`VECTORWARP_NODE_ID` and, optionally, `VECTORWARP_NODE_LABEL` on each API
service. Every proxied response carries that identity; a missing or changed
identity is rejected. Node additions and removals made in Settings take effect
immediately and are written atomically. No addresses or credentials belong in
the public application tree.

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
