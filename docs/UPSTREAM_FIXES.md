# Upstream fixes carried into VectorWarp

VectorWarp is based on blah2's Kraken multi-channel branch. Existing upstream
pull-request branches are not rewritten by this integration.

| Submitted fix | VectorWarp coverage |
| --- | --- |
| [adsb2dd #5: position sample timing](https://github.com/30hours/adsb2dd/pull/5) | Uses `feed.now - seen_pos` for motion derivatives and compares duplicate positions against stored processing history. |
| [adsb2dd #3: inactive configuration cleanup](https://github.com/30hours/adsb2dd/pull/3) | The separate converter/configuration cache is gone. Its replacement expires bounded per-aircraft histories and clears them when configuration changes. |
| [blah2 #48: bounded track history](https://github.com/30hours/blah2/pull/48) | Limits browser trails to 100 points, preserves lifetime counts, and keeps 255 internal states so every supported M-of-N confirmation window still works. |
| [blah2 #46: ADS-B health](https://github.com/30hours/blah2/pull/46) | Integrated raw-feed health, conversion warmup, stale-feed and connection errors; hidden when disabled, live truth suppressed during replay. |
| [blah2 #45: multi-channel Kraken](https://github.com/30hours/blah2/pull/45) | Already the integration base; capture/replay and settings now support 2–8 channels. |

## Additional correctness repairs

- ADS-B geometry uses WGS84 positions and geometric altitude in meters, rejects
  invalid/stale/out-of-order samples, and avoids a fabricated zero in its median
  Doppler history. Recent valid overlays survive repeated polling of one position.
- Track association uses the predicted delay and Doppler, preserves matched
  measurements, assigns each detection at most once, and ages only missed tracks.
  Removing tracks also removes their counters without skipping neighboring tracks.
- Track prediction uses the radar convention that positive Doppler means decreasing
  bistatic path length. Both velocity and acceleration terms include wavelength.
- Reference spectra use the actual sample rate and FFT bin spacing, correct odd/even
  shifts, normalized FFT amplitudes, and `20 log10(amplitude)` levels. Zero input
  produces a finite display floor, and FFT storage is released on destruction.
- TCP output completes short writes instead of silently dropping part of a JSON
  frame. Connection errors reach the processing error handler.
- Socket contexts now outlive their sockets during shutdown. AddressSanitizer
  caught this integration lifetime bug during replay acceptance.
- Silent maps have a finite display floor, and coordinate conversion validates
  JSON before modifying it. CFAR includes valid training cell zero and handles
  an empty training window without dividing by zero.
- Centroid neighborhoods work across delay zero. Interpolation checks its indices,
  preserves finite flat/edge detections, rejects non-peaks, and avoids undefined
  sub-bin estimates or overwriting the wrong peak level.
- Recording rejects discontinuities, empty/incomplete files and mismatched replay
  metadata; revisioned recording requests distinguish a new recording from an old one.
- Automatic GPU selection checks sustained frame costs, including input retirement,
  and returns to CPU when the GPU no longer beats the qualification baseline.

Focused C++ and Node tests cover these changes. The recorded-IQ performance
campaign used frozen earlier source, so its numbers do **not** benchmark these
later repairs. See [the benchmark report](RECORDED_IQ_BENCHMARK.md).

ADS-B remains a display/evaluation overlay, never a detection or tracking input.
These repairs do not establish bearing reliability or prove tracker accuracy on
real aircraft. The adapted adsb2dd code retains its [MIT attribution](../api/third_party/adsb2dd/PROVENANCE.md).
