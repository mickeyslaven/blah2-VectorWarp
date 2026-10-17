# External tracker shadow feed

VectorWarp can optionally copy detection frames to an experimental tracker over
a newline-delimited TCP stream. This is a shadow path: the built-in detector,
GPU tracker, bearing processing, and Retina output remain authoritative and do
not wait for or consume the sidecar's result.

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

truth:
  adsb:
    delay_tolerance: 2
    doppler_tolerance: 5
```

Each detection receives either a strict delay/Doppler ADS-B match or `null` in
the parallel `adsb` array. These annotations are inputs only to the external
tracker. They never change CFAR detections, built-in associations, bearing
estimates, or the existing Retina track stream.

Inspect `/api/tracker-forward` for connection state, frames received and sent,
pending-frame overwrites, ADS-B matches, the most recent send time, and errors.
An increasing `overwritten` count means the shadow consumer or ADS-B lookup is
not keeping pace; it does not imply a dropped radar CPI.
Incomplete detection-stream messages are rejected and counted as `invalid`
instead of being allowed to terminate a strict external consumer.
