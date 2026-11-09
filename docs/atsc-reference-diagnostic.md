# ATSC reference pilot diagnostic

An ATSC 1.0 lane may set `atsc_reference_mode` to `raw` or `pilot_aided`
(default). Pilot-aided mode always keeps the original physical reference for
direct-signal/clutter cancellation. It projects only the coherently measured
carrier pilot from the matched-filter reference after a disjoint held-out
suppression test. Any unavailable metric, incoherent pilot, excessive
frequency offset, unstable held-out amplitude, non-finite sample, or excessive
removed-power fraction selects raw IQ for that CPI. It is not full waveform
reconstruction. A requested
`decision_directed` mode, an unknown mode, a pilot mode on a non-ATSC lane,
an array reference, or a lane whose sample rate excludes the nominal pilot is
rejected. The estimator consumes the existing channelized output from either
CPU or Vulkan. Pilot measurement examines at most 16 consecutive approximately
0.25 ms blocks per CPI.

The optional frame `atsc_reference` object reports `requested_mode`,
`effective_mode: raw` or `pilot_projected`, the fallback reason, candidate status, frequency offset,
coherence, pilot-to-residual ratio and the inclusive input sample positions,
decimation and channelizer group delay of the measurement. Positions are
channelizer output provenance: subtract group delay for RF sample time, and
consult the surrounding frame's capture-position availability and epoch.
Measurements need all 16 blocks; short CPIs, silence or non-finite input result
in unavailable metrics. A discontinuity resets the radar and measurement.

Projection reports its independent held-out suppression and removed-power
fraction. The physical reference remains untouched. The projected buffer is
used only by ambiguity/matched filtering, including the zero-copy borrowed
clutter path; surveillance IQ and raw cancellation coefficients are unchanged.
On failure the candidate is not partially applied.

The metric assumes an ATSC 1.0 8-VSB channel centered at the configured
frequency, with the pilot nominally one quarter of the 10.7622378 MHz symbol
rate below center. This frequency estimate is relative to that nominal pilot,
includes tuner/transmitter offset, is ambiguous outside roughly +/-2 kHz, and
has not been calibrated against recordings. The ratio is estimated from block
correlation and residual energy, not RF SNR. A narrowband interferer can create
a candidate, which is why held-out stability and removed-power gates are
mandatory. A pilot alone does not measure symbol timing, so timing is
explicitly unavailable. Projection subtracts the qualified deterministic pilot
component; it does not rotate either input or change surveillance timing.

Reproduction requires the same lane mode, rate, frequency, channelizer and
sample stream. This feature does not alter recorded IQ. Before a live trial,
validate pilot frequency and false-candidate behavior on labeled captures,
sample provenance across gaps and replay, and CPU/Vulkan agreement.

The CPU microbenchmark can be run without hardware:

```
c++ -O3 -std=c++17 -Isrc test/benchmark/AtscPilotBenchmark.cpp -o /tmp/atsc-pilot-bench
/tmp/atsc-pilot-bench
```

For a completed full-passband BLAH2IQ recording, the bounded offline pilot
checker reads only the first recording block and measures each standards-
defined pilot directly in the wideband reference channel. It does not modify
the recording or claim demodulator lock:

For the next offline gate, `script/analyze-atsc-field-sync-recording.py` reads a
bounded portion of a completed BLAH2IQ recording, isolates one requested 6 MHz
lane, acquires the pilot, searches symbol timing and carrier phase, requires the
complete field-training sequence, and reports held-out equalizer correlation
and EVM. A pilot or segment score alone never produces `locked: true`.

```
python3 script/analyze-atsc-pilot-recording.py recording.blah2iq \
  --reference-channel 1 --lane-rate 6144000 \
  --frequency 587000000 --frequency 593000000 --frequency 599000000
```

The direct wideband measurement intentionally avoids allocating a channelized
lane; the production estimator independently checks the pilot after the live
channelizer. A tone near the nominal pilot is necessary but not sufficient for
8-VSB field, timing, equalizer or decision lock.

The separate `script/benchmark-atsc-reference-bound.py` simulates a complex
Gaussian waveform and reference noise; it is an oracle upper bound, not ATSC
reconstruction or a throughput benchmark. At 0 dB reference SNR its ideal
reference has about 3.01 dB greater coherent efficiency than the noisy one.

## Offline reconstruction components

`script/atsc-offline-components.py` contains an independently tested bounded
windowed-sinc fractional resampler and a supervised complex FIR equalizer.
The latter requires a known, already aligned training sequence; it is not an
ATSC field-sync detector. The resampler is generic and does not implement the
ATSC VSB transmit response. Neither component creates an ATSC reference.
Training uses at most 65,536 samples and 32 taps. The resampler limits its
temporary coefficient matrix by chunk size and flags incomplete filter support.
The nominal 10.7622378 Msymbol/s clock maps exactly to the 6.144 MS/s
lane grid at the rational ratio 64125/36608; the utility supplies nominal
positions with a caller-supplied timing offset. It does not estimate timing,
clock drift, VSB pulse shaping or the necessary anti-alias filter.

The same script compares an externally generated candidate with raw lane IQ.
Inputs are equally sized, already aligned, one-dimensional complex `.npy`
arrays. They are memory-mapped and evaluated one block at a time (at most
4,194,304 samples per block). Supply the original lane sample rate and the
capture index and stride of its first sample; the tool reports inclusive input
indices for each block. Candidate confidence is a JSON array with one number
between zero and one per block; missing or low confidence, or a non-finite or
zero candidate, selects the raw reference. Confidence is supplied by the
candidate producer and is not independently validated by this tool.

```
python3 script/atsc-offline-components.py --raw reference.npy \
  --candidate reconstructed.npy --surveillance surveillance.npy \
  --confidence confidence.json --sample-rate 6144000 \
  --first-input-sample 0 --input-stride 4 --block-samples 262144 \
  --delay 47 --doppler-hz 42
```

For each reference, a separate scalar direct-path coefficient is fitted on the
first half and applied to the second half. The output gives held-out residual
power and a single complex, normalized CAF cell at the requested delay and
Doppler. This is not the production multi-tap clutter canceller or a detection
performance comparison; coefficients fitted on noisy references are biased.
Choose target and control cells independently of the results, account for
channelizer group delay and gaps when preparing files, and check sample identity
and the input rate against the recording metadata. The tool cannot verify that
the three arrays actually share a clock or sample alignment.

The missing stages for a real prototype are carrier acquisition and tracking
with unambiguous frequency, joint fractional symbol timing and rate recovery,
validated ATSC 1.0 field-sync recognition, channel/equalizer acquisition using
that sync, calibrated symbol-decision confidence (and ideally FEC), and
standards-correct VSB pulse-shaped remodulation with carrier and receive-channel
phase restored onto the original lane sample grid. An end-to-end synthetic
8-VSB transmitter and labeled recordings are needed before the candidate
confidence or CAF comparisons can be interpreted as ATSC reconstruction.

ATSC 1.0 pilot and waveform characteristics: [ATSC A/53 Part 2](https://www.atsc.org/wp-content/uploads/2015/03/a_53-Part-2-2011.pdf).
