# External tracker feed and safe publication

VectorWarp can copy detection frames to an external tracker over a
newline-delimited TCP stream. The feed is unassisted when `annotate_adsb` is
false: every ADS-B annotation is `null` and no truth endpoint is read. Native
tracking, bearing processing, and Retina detection output never wait for the
sidecar.

The forwarder keeps at most one pending frame. If ADS-B lookup or the sidecar is
slow, an older pending frame is overwritten instead of building a backlog or
delaying radar processing. A disconnected sidecar is retried every five seconds.

Enable the feed explicitly:

```yaml
network:
  tracker_forward:
    enabled: true
    host: 127.0.0.1
    port: 30100
    annotate_adsb: false

truth:
  adsb:
    delay_tolerance: 2
    doppler_tolerance: 5
```

`annotate_adsb: true` preserves the original research mode, where each
detection receives either a strict delay/Doppler ADS-B match or `null` in the
parallel `adsb` array. These annotations are inputs only to the external
tracker. They never change CFAR detections or bearing estimates.

For a same-input control that cannot use ADS-B assistance, configure a second
sink. It receives the same validated detection frames with an all-null `adsb`
array and maintains independent connection and overwrite accounting:

```yaml
network:
  tracker_forward:
    enabled: true
    host: 127.0.0.1
    port: 30100
    unassisted:
      enabled: true
      host: 127.0.0.1
      port: 30101
```

Inspect `/api/tracker-forward` for connection state, frames received and sent,
pending-frame overwrites, ADS-B matches, the most recent send time, and errors.
An increasing `overwritten` count means the shadow consumer or ADS-B lookup is
not keeping pace; it does not imply a dropped radar CPI.
The unassisted sink's corresponding counters are returned under `unassisted`.
The original numeric CPI timestamp is preserved when the API normalizes its
framed JSON stream for the sidecar. Incomplete or malformed messages are
rejected and counted as `invalid` instead of being allowed to terminate a
strict external consumer.

## Publish an unassisted tracker safely

An external tracker can write bounded JSONL events to a file readable by the
VectorWarp API. `hybrid` publication makes those stable tracks primary and
adds persistent native-only tracks that are outside the configured
delay/Doppler deduplication gate. A healthy empty external tracker produces
only those persistent native supplements. If its detection input disconnects,
`/api/tracker` automatically falls back to the complete native frame.

```yaml
process:
  tracker:
    publication:
      mode: hybrid
      event_file: /var/lib/vectorwarp-api/tracker/unassisted.jsonl
      stale_after_s: 3
      native_min_associations: 5
      delay_tolerance: 2
      doppler_tolerance: 5
```

`native` is the backward-compatible mode. `external` publishes only fresh
external tracks, with the same stale fallback. `/api/tracker/native` always
exposes the untouched processor frame, and `/api/tracker/publication` reports
freshness, input errors, deduplication, supplementation, and fallback state.

External event detection delays and their history are in kilometres. Publication
converts them to the native API's sample bins using `capture.fs * 1000 / 299792458`
before deduplication. Publication `delay_tolerance` is in sample bins. A missing
or invalid sample rate rejects external events and falls back to the complete
native frame.

The external event file is tailed asynchronously and never enters the CPI
processing path. Start it empty or allow the reader to reach EOF; VectorWarp
intentionally ignores old history when it first opens a file. Give the API
read access without giving the tracker write access to configuration or radio
state. Rotation by rename or truncation is detected automatically.

Disabling ADS-B annotation here does not require disabling VectorWarp's truth
source. A site may keep ADS-B enabled for calibration, evidence recording, and
offline evaluation while the operational tracker remains fully unassisted.
