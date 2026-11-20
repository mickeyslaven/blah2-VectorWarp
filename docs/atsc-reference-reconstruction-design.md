# Hardware-agnostic ATSC reference reconstruction

## Purpose and safety contract

VectorWarp needs a cleaner passive-radar reference, not television pictures.
The reconstructor accepts a channelized ATSC 1.0 reference plus any number of
sample-synchronous surveillance streams. It must preserve RF sample identity,
relative phase, carrier history and channelizer group delay. It may never
silently substitute an unlocked or low-confidence waveform: every CPI selects
an explicitly qualified reconstructed reference or the unchanged raw reference.

The design is independent of receiver brand, CPU architecture and capture
sample rate. Receiver adapters provide complex samples and provenance; the
reconstructor derives rational rate conversion from the actual lane rate and
the ATSC symbol rate. Multiple surveillance channels reuse one reconstruction
for their illuminator rather than repeating demodulation.

## Production pipeline

1. **Carrier acquisition:** extend the existing pilot estimator into a bounded
   FLL/PLL. Apply the same carrier rotation to the reference and every
   synchronous surveillance stream so bistatic phase and Doppler are preserved.
2. **Matched filtering and timing:** use the ATSC root-raised-cosine response
   (`alpha = 0.1152`) and a rational polyphase synchronizer to recover the
   10.7622378 Msymbol/s grid from any sufficient complex lane rate.
3. **Field synchronization:** correlate the standards-defined segment sync,
   PN511 and PN63 field sequences. Field sync supplies independently known
   training symbols and prevents an arbitrary narrow tone from claiming lock.
4. **Equalization:** acquire a feed-forward/decision-feedback equalizer from
   field-sync training, then adapt decision-directed between field syncs.
5. **Qualified symbol decisions:** slice the eight terrestrial VSB levels and
   publish EVM, field-sync correlation, equalizer error, lock age and cycle-slip
   evidence. Content/FEC decoding is optional validation; it is not on the live
   radar path.
6. **Coherent remodulation:** reinsert pilot and sync, apply the standards-
   correct pulse shape, return to the original lane sample grid, restore the
   received carrier phase and preserve exact input-sample/group-delay mapping.
7. **Per-CPI selection:** use reconstruction only after sustained field and
   timing lock, bounded EVM, continuous provenance and a predeclared offline
   improvement test. Any gap, discontinuity, ambiguity or confidence failure
   selects raw samples for that CPI.

## Reusable components

- **liquid-dsp** is the preferred production building-block library. It is
  X11/MIT licensed and provides rational resampling, polyphase symbol timing,
  NCO/PLL and LMS equalizer primitives. It is packaged for Fedora, Debian/
  Ubuntu and Homebrew, including Linux and macOS on ARM64 and x86-64. VectorWarp
  must pin and source-close one reviewed version like its other dependencies.
  ATSC framing, confidence policy, phase restoration and remodulation remain
  VectorWarp code because liquid-dsp is not an ATSC receiver.
- **GNU Radio gr-dtv** already contains a complete ATSC receive pipeline and is
  valuable as an offline interoperability oracle. It is GPL-3.0-or-later and a
  large runtime, so it must not be copied, linked into, or made a required part
  of the MIT-licensed VectorWarp binaries. An optional user-installed external
  oracle may exchange recording files/results through a documented interface.
- Existing VectorWarp channelizers, fixed executors, recording provenance and
  raw-reference fallback remain the production transport. No demodulator may
  own SDR acquisition or create a receiver-specific processing fork.

Primary references: [ATSC A/53 Part 2](https://www.atsc.org/wp-content/uploads/2015/03/a_53-Part-2-2011.pdf),
[liquid-dsp](https://github.com/jgaeddert/liquid-dsp),
[liquid-dsp symbol synchronizer](https://liquidsdr.org/doc/symsync/), and
[GNU Radio gr-dtv](https://github.com/gnuradio/gnuradio/tree/main/gr-dtv).

## Efficiency limits

- Persist carrier, timing and equalizer state across CPIs; allocate no full-CPI
  scratch buffers and never demodulate the wideband input separately per lane.
- Run each reconstructor once at its channelized lane rate, then share its
  immutable result with all surveillance channels and downstream CAF workers.
- Keep field-sync search bounded around predicted positions after acquisition.
  Reacquire globally only after a declared discontinuity or sustained lock loss.
- Use SIMD CPU polyphase/equalizer kernels first. Batch independent illuminator
  lanes on the existing fixed executor. Move FIR/resampling to the GPU only
  after benchmarks show transfer-free resident processing is faster; do not add
  device copies merely to claim GPU use.
- Expose stage timings, queue depth, fallback count and reconstruction reuse in
  health telemetry. Processing admission must reject a configuration whose
  measured worst-case cost cannot meet the CPI cadence.

## Configuration and capability model

`atsc_reference_mode` remains per illuminator:

- `automatic`: default for ATSC 1.0 and ATSC 3.0; use a candidate only after
  its per-channel held-out and target-preservation gates pass, otherwise
  publish raw plus the rejection reason.
- `raw`: unchanged reference.
- `pilot_aided`: experimental offline ATSC 1.0 pilot projection; live
  configuration rejects it until the native path meets the acceptance gates.
- `decision_directed`: available only in builds with the reviewed reconstruction
  backend and only after runtime lock. Until then configuration validation must
  reject it rather than silently behaving like `raw`.

Node federation advertises supported reference modes independently of receiver
type. Recordings retain full-passband IQ, all lane definitions, stage metrics,
exact fallback decisions and truth timelines so another installation can
reproduce the decision.

## Acceptance before enabling `decision_directed`

1. Standards-vector acquisition, timing, field-sync, equalizer, remodulation and
   discontinuity tests pass on every supported architecture.
2. GNU Radio is used only as an external oracle to cross-check lock/symbol
   timing on redistributable synthetic vectors.
3. Labeled real recordings cover strong, weak, multipath, adjacent-channel and
   false-pilot cases. Raw and reconstructed references use identical held-out
   CAF/clutter/detector scoring with predeclared cells and denominators.
4. Reconstruction improves held-out target sensitivity or clutter residuals
   without moving known delay/Doppler, increasing false tracks, missing cadence,
   retiring samples or adding capture drops.
5. CPU and GPU routes produce equivalent provenance and remain within the CPI
   budget at the maximum admitted number of illuminators. Failure at any stage
   is a visible raw fallback, never a degraded radar mode.

## Implemented experimental stages

The repository now contains standards-exact segment/PN511/PN63 field
synchronization, held-out training validation, a continuity-aware symbol
reconstructor, and a bounded completed-recording acquisition tool. The selector
requires consecutive locks, checks independently held-out EVM, preserves sample
provenance, and returns the unchanged raw symbols after a gap, corrupt training,
or failed quality gate. The offline tool isolates a single 6 MHz lane before
carrier/timing recovery so adjacent illuminators cannot create false confidence.

These stages remain **experimental offline**. Exact-grid pulse shaping and
remodulation, held-out CAF/clutter improvement, and maximum-lane cadence are
still required by the acceptance list above. Configuration therefore continues
to reject `decision_directed`; passing field lock is not silently treated as
permission to alter the live radar reference.

### ATSC 3.0 soft reconstruction

The experimental ATSC 3.0 path retains the LDPC decoder's converged soft
beliefs after an independently valid LDPC syndrome and BCH remainder. It maps
those beliefs to the per-cell posterior constellation mean and a bounded
deterministic-energy confidence. Pilots have confidence one; an undecoded FEC
block has confidence zero and therefore retains its measured raw carrier.

Offline symbol-native qualification compares the hard reconstruction, the
posterior mean, a measured-channel/phase-restored posterior, and bounded
confidence-weighted physical-reference mixtures against the identical raw IQ.
Selection and holdout partitions both include decoded-waveform and measured-
waveform target controls. No posterior candidate becomes live merely because
FEC passed: it must reduce target-normalized background on both partitions and
independent recordings while respecting the target-response and false-peak
limits. This distinction is important because FEC certainty does not model
transmitter nonlinearities, oscillator residuals, or the propagation channel.
