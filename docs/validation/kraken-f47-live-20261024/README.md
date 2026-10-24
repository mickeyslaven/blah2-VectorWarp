# Kraken f47 bounded live hardware gate, 24 October 2026

The content-addressed x86_64 candidate built from KrakenSDR Suite
`f47ed2fb48ab5dba888b7d45fd08c9745ae22199`, Kraken RTL-SDR
`578b43ddd971786e84e2a6ab3c338ee48c8739c3` and the four VectorWarp
companion patches passed a bounded test on the five-channel Fedora Kraken
host. The candidate Heimdall SHA-256 was
`14b7006ca1d668cb5b37fbd66f19962d1d007836d6ce5bac7cdfcb1bb3cfe763`;
the driver was statically linked. `candidate-build.json` records every source,
archive and patch digest.

## Result

The canonical config SHA-256 stayed
`7a61e0abb613cef8895c0e9fbc596f49f63705f4331d26816daafcfc79a489e0`.
Control and wire readback remained 527 MHz, 2.4 MS/s, five coherent channels
and gains `[2.7,2.7,1.4,1.4,3.7]` dB. Calibration converged with phase state
4, calibration noise off, no recovery/reconfiguration, no coherence event and
zero ADC rail samples on all five channels.

During the 20-second wire observation, 2,931 MCHT frames carried 48,021,504
samples per channel. Frequency-change counter and group stayed fixed; frequency,
gain and channel metadata matched every frame. Acquisition epoch stayed fixed,
while first-sample, transfer and packet sequence fields advanced strictly. The
first-to-last deltas were 48,005,120 samples and 2,930 transfer/packet sequences;
monotonic time advanced 20.002089471 seconds and TAI/realtime advanced
20.002322677 seconds. Every timing anchor covered the full published frame.
This qualifies the native MCHT-v1 hardware path; the restored former receiver
correctly emitted legacy MCHQ without the optional extension.

## Mandatory rollback

The candidate ran from 22:46:08Z to 22:46:56Z and was then stopped even though
it passed. By 22:47:32Z the prior Heimdall, API and processor were all active.
The running and installed prior Heimdall both hashed
`1eff9c21c65370956ff4b2e037484f486aa2abc1da186187c5bc42cbe4a4a546`;
the original systemd unit and port drop-in hashed `6c994038...` and
`469d59b9...`. A fresh post-rollback observation accepted 733 converged frames
and 12,009,472 samples per channel at the exact saved frequency and gains.
The candidate unit was inactive and no candidate remained deployed.

`attempt2/candidate-live-summary.json` and
`attempt2/rollback-live-summary.json` are the machine-readable bounded results.
The hash, linkage, USB, service and selected journal receipts sit beside them.
`live_gate.py` is the exact validator. The first attempt is retained separately:
SELinux rejected the staged file's inherited `tmp_t` label before it executed;
the rollback completed, the candidate was given the normal `/opt` `bin_t` label,
and the successful attempt followed. No candidate or RF failure occurred in
that preliminary attempt.

The direct hardware timing path improved from unextended MCHQ to validated
MCHT-v1. API/processor event-receipt latency was not compared during candidate
ownership because those consumers were intentionally quiesced to preserve a
single bounded receiver owner; the existing software integration simulations
cover that protocol consumer path.
