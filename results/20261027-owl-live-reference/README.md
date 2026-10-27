# Live OWL external-reference qualification

## Current result

The OWL reference transport, HP recorder and independent-clock synchronizer are
working.  This does **not** yet authorize replacing the live synthesized
reference; the same-IQ detector A/B remains the promotion gate.

- OWL remains at 551 MHz WTAE, 2 MS/s and a 0.5 s CPI.  Its native configuration
  SHA-256 remains
  `a900508d525276c4b13b9719b330eae1b70c4ec97dad168e7463fc562f983539`.
- The deployed exporter image is
  `vectorwarp/owl-blah2:external-reference-v5`, image
  `sha256:f1388defee62548717a710aa48316605579caac7d7df0a67f1fd6ee3d4274679`;
  binary SHA-256
  `0e8cd063a65d7b4e8df04e1b526a992e3a9768d3e03e8960701d5321ff0d0ed0`.
- HP records only OWL channel A on the Ford Street LAN at
  `192.168.1.210:4096`.  The sealed ledger and segments are under
  `/var/lib/vectorwarp/external-reference/owl-551`.  Host arrival is explicitly
  not RF time.
- A warmed 201-CPI profile measured 470.74 ms median, 498.58 ms p95 and 8/201
  CPIs at or above 500 ms.  The stock matched profile measured 464.63 ms,
  496.46 ms and 9/201 respectively.  The exporter added about 7.59 ms to median
  extraction cost without worsening the observed 500 ms tail count.

## Real waveform lock

HP was temporarily tuned to 551 MHz for one simultaneous capture, then restored
to the exact 509 MHz config revision
`dba6116dd9dd671bfd3863bddccd415083ef4b77a0ba6d4c3c6648087d7edfb3`.
The valid Kraken source clip is
`kraken-clip-r1-1793088886162896882-1.mchq`, SHA-256
`71ae62341daa7d2643eaf81e6bc1414286cd7548b401fb7c609a5ed802942773`.
Its aircraft admission was quarantined for a missing truth bracket; that does
not invalidate waveform synchronization.

`qualification-551-full-cpi-20261027.json` proves an independent-clock lock:

- Kraken pilot channel: 2
- sample-clock offset: +3.333333 ppm
- affine rate: 0.8333361111 OWL samples per Kraken sample
- full-CPI CFO: about +1758.96 Hz
- seven 450 ms locks: 0.504 through 0.637 coherence
- median full-CPI coherence: 0.605879

The short-window CFO estimate was about 1.1 Hz coarser than the long-CPI value;
using it would reduce 500 ms coherence to roughly 0.33.  The replay builder
therefore uses the median affine-CPI CFO, not the short-lock FFT bin.

## Six-input replay

`script/build-external-reference-replay.py` produces a BLAH2IQ input with OWL
as channel 0 and all five original Kraken channels as channels 1--5.  It uses
the qualified affine clock map, long-CPI CFO and per-CPI constant phase/RMS
normalization.  It fails closed on frequency, rate, epoch or qualification
mismatch.

The current replay SHA-256 is
`52caf328d1b4d4e416ea759b672ef4948d2d309ee433e358227d2f8fbad3c638`;
its median 500 ms reference/pilot coherence is 0.607234.  The paired A/B uses
identical surveillance bytes and compares the external dedicated reference to
the five-channel synthesized reference with the same detector and false-alarm
configuration.

The first same-IQ A/B used a weak aircraft pass beyond 40 km.  Neither arm
accepted the truth-selected cell.  The synthesized-reference arm had +0.224 dB
median fused target contrast and 24 nuisance detections; the OWL arm had
-1.287 dB and 20 nuisance detections.  This rejects promotion from this pass:
OWL reduced the observed nuisance burden, but did not recover the aircraft.
The derived replay is retained on the evidence host and can be regenerated
from the immutable raw inputs; the local duplicate was removed for disk space.

## Transport repair

The original three-CPI sender queue and synchronous 480 MB segment seal caused
real export gaps.  The accepted path now has a 16-CPI sender queue, retries a
frame across transient disconnects, and uses protocol schema 2 acknowledgements
so a frame is not released until HP accepts its exact sequence.  HP recognizes
and re-acknowledges an identical retry without writing it twice.  Segment hash,
flush, rename, receipt and retention work runs on a background sealer, and the
durability batch is 10 seconds (80 MB) instead of 60 seconds (480 MB).

The OWL config remained byte-identical at
`a900508d525276c4b13b9719b330eae1b70c4ec97dad168e7463fc562f983539`.
Only the `blah2` container was recreated.  During a controlled HP recorder
restart, schema-2 sequences 80 and 81 remained contiguous; OWL reported two
retry failures but zero drops, and the restarted recorder reported zero sample,
sequence or invalid-frame gaps.  The post-restart acceptance then reached 303
recorded frames through more than 30 ten-second seal boundaries with all three
gap/error counters still zero; the OWL exporter reached 360 submissions with
zero drops (four frames remained normally in flight) and no container restart.

## Promotion state

The transport and independent-clock alignment are qualified for shadow
collection.  Production still uses the Kraken synthesized reference.  A live
external reference would also require HP to use the same 551 MHz illuminator
and the coherent beam bank to calibrate its five surveillance-channel phases
against the independent reference.  The first same-IQ aircraft A/B did not
show a sensitivity gain, so silently substituting OWL in production would be
an unqualified detector change.

## Safety history

An initial attempt to compile on the 2 GiB OWL host coincided with the stock
processor crashing and restarting.  Compilation was stopped and moved to an
isolated ARM64 Debian VM.  A separate stale HP diagnostic process had consumed
one CPU since October 23 and entered heavy I/O during recording; only that
orphan was terminated and the processor recovered.  Neither incident is part
of the accepted signal path.
