# Ford Street miss recovery: bounded next method

## Evidence and decision

The 2026-10-21 ledger records r94/N237LP at 2.238 dB local integrated ratio, 0.438 dB below the matched-control p99, with no truth-near detection; r12/N4313D is a matched 21 km / 80 Hz independent miss. The positive control r277/N536ME has a persistent production track. A prior 861-path *linear*, blind motion bank was rejected; higher PFA, CFAR variants, direct-path nulls, reference changes, whitening, clutter changes, covariance smoothing, and diagonal loading are also rejected. Therefore none should be repeated.

## Research and mapping

| Candidate | Primary source | Current seam | Cost / false-alarm implication | Rank |
| --- | --- | --- | --- | --- |
| ADS-B-gated quadratic delay/Doppler integration with matched local controls | [Zheng et al., DP-TBD](https://doi.org/10.1049/iet-rsn.2015.0332) derives trajectory-conditioned DP-TBD scoring; [Guan et al.](https://doi.org/10.1109/ICOSP.2014.7015331) addresses PBR TBD thresholds | New offline `script/analyze-accelerated-motion-truth.py`, consuming the already exported replay maps | 3x3 velocity x 3x3 acceleration = 81 paths/anchor by default, but the same bank runs at every local control. O(frames × paths × controls); cache bounds repeated cell reads. No live output. | 1 |
| Reduced-rank / regularized ECA | [ECA least-squares formulation](https://doi.org/10.1186/s13634-016-0431-2); [DVB-T ECA-C study](https://eurasip.org/Proceedings/Eusipco/Eusipco2018/papers/1570439308.pdf) | `src/process/clutter/WienerHopf.*`, `src/process/ambiguity/Acceleration.cpp` | Requires a new cancellation basis and validation that it does not null the target. Existing ECA covariance smoothing and loading were null/rejected; a fresh rank-selection implementation needs GPU/C++ build and more replay matrix. | 2, only after a positive analysis signal |
| Phase-compensated coherent CPI integration | [Zuo et al., passive-bistatic long coherent integration](https://doi.org/10.1049/rsn2.12025) estimates acceleration and compensates the quadratic phase term | Complex CAF samples would be needed before `core/GpuPostprocess.private.h` converts the fused map to power | Current replay artifacts retain display power, not complex per-cell CPI phase. Implementing this now would be untestable from the retained maps and risks uncalibrated phase trials / false-alarm inflation. | 3, not justified now |

The selected method is intentionally **not** a broader blind motion bank: it adds curvature (constant acceleration), constrains it to a small physical bank around the ADS-B trigger anchor, and requires the identical search at nearby delay/Doppler controls. It is replay/offline-only and labels every result `candidate_only`; it cannot claim a radar recovery without a persistent truth-near detection/track in an independent replay.

## Implementation and acceptance

`script/analyze-accelerated-motion-truth.py` uses exported ambiguity-map power and computes a per-CPI local power/background ratio along `x(t)=x0+v t+½a t²`. It reports the winning kinematics, per-frame evidence, and margin over the control p99. It does not feed ADS-B into live inference. The compact unit test constructs a curved target path and verifies the quadratic term wins while controls remain separate.

Run one job at a time on CASS, using pre-existing map replays (no IQ replay required):

```sh
python3 tools/analyze-accelerated-motion-truth.py \
  --replay results/r94-baseline-tbd-m3n5.json --truth results/r94-truth.jsonl \
  --output results/r94-accelerated-local-v1.json
```

Decisive matrix: r94 and r12 misses, r277 positive control, and the r12 20-frame exact-truth negative/control path. Accept only if r94 or r12 has at least +3 dB over the matched control p99, multi-frame continuity, and the positive control remains selected without comparable control-path growth. Otherwise reject the candidate and do not promote it to GPU/live code.

At the native r94 map dimensions (601 × 511) this script stores only the already-loaded JSON maps plus one scalar cache entry per inspected cell; the default 81 paths × at most 80 controls × 20 frames is well under the CASS memory budget, and it is explicitly run serially.

## CASS result: reject

The serial retained-map matrix wrote `results/*-accelerated-local-v1.json` on both CASS and this worktree. r94 scored **-0.948 dB** below its matched control p99; r12 **-0.687 dB**; and r13 **-1.118 dB**. The r277 persistent-track positive control scored **-10.086 dB**, so the retained raw-map analysis does not even retain the known positive. This fails the acceptance contract twice: no independent truth-near recovery and no positive-control retention. The candidate is rejected and must not be promoted to GPU or live code. The concise matrix is `results/20261021-accelerated-motion-matrix.json`.

## Calibrated complex replay and recovery boundary

A replay-only complex CAF cutout recorder and synthetic echo injector now make the
acceptance harness trustworthy. The r277/N536ME positive replay reproduces seven
truth-near detection frames and an active production track. Its measured scoring
offset is +0.0258 km / +7.05 Hz, and its calibrated 8-CPI cell clears the resident
TBD CFAR threshold by +0.98 dB. The same calculation places r12/N4313D 5.37 dB
below threshold. Its selected beam changes almost every CPI, consistent with noise
rather than a stable spatial echo.

Synthetic injection into the untouched r12 background recovers an active track
through -88 dB and collapses by -90 dB. This proves that the detector, tracker,
geometry, and replay scoring can recover a real echo when one is present.

The strongest post-CFAR candidate was the geometric mean of per-frame local ratios
over the bounded 81-path quadratic bank. It put r13/NWF412 0.110 dB above 64
matched controls while retaining r277 by 3.026 dB. That result is rejected:
widening the bank to 1,225 plausible paths moves r13 to -0.376 dB and reduces r277
to +0.891 dB; a deterministic 1,000-anchor whole-map sample also finds three
unrelated local maxima above r13. It cannot meet the +3 dB acceptance contract or
a global false-alarm budget.

Longer coherent dwell does not recover r13. At a 1.0 s CPI with 2-of-3 initiation,
r13 produces no truth-near plots, while r277 remains an active truth-near track.
A 2.0 s r13 replay also produces no truth-near plots. Angle-constrained TBD was
rejected because sparse compatible training cells collapse the CA-CFAR noise
estimate and flood the tracker; noncoherent and noise-whitened fusion do not reveal
the target either.

Decision: these miss recordings do not contain enough independently qualified
radar evidence to claim a blind recovery. Do not lower live thresholds or promote
the post-hoc motion statistic. Retain the calibrated complex recorder, echo
injector, and global-tail analyzer as the acceptance harness for longer captures.

The final check used the newly captured r623 clip for the newest confirmed miss,
hex a1f850. It also has zero truth-near plots/tracks. Its calibrated 8-CPI cell is
5.66 dB below resident CFAR and 1.73 dB below the complex-cutout control p99 at
the event's initial truth cell. Those point estimates are retained as historical
results but superseded by the signal-time trajectory correction below. The blind
geometric path score was 1.00 dB below matched controls. The corrected trajectory
test independently confirms the same limiting condition in fresh IQ: there is no
separable radar echo to recover without accepting many unrelated peaks.

## Acquisition loss budget and signal-time truth correction

The raw-MCHQ loss audit compares r277 with r623 using identical measurements. Both
recordings are calibrated (`phaseState=4`), have stable metadata, zero sample-lag
span between channels, no meaningful clipping, and about 0.005–0.010 rad of
block-to-block relative phase dispersion. The array principal component contains
97.7% of r277 power and 96.7% of r623 power. Held-out common-signal prediction is
27–32 dB for r277 and 27–30 dB for r623. The receiver is therefore not losing the
miss through clipping, a sample slip, or broken array coherence.

The clips contain a 2.4 MHz complex slice of a 6 MHz television channel. Complete
ATSC waveform reconstruction is impossible from these stored samples; 60% of the
channel was never captured. A full decoder/remodulator is therefore not a valid
software-only recovery path for these artifacts.

The retained eligibility events exposed an error in the earlier offline oracle:
the event's initial delay/Doppler was reused throughout a later clip. New
`vectorwarp-clip-truth-trajectory-v1` artifacts interpolate the retained ADS-B
position samples at each CPI's signal time. For example, r12 moves from 21.44 km /
-33.7 Hz to 22.19 km / -117.8 Hz during its ten-second clip; r623 moves from
42.29 km / -151.4 Hz to 43.12 km / -152.1 Hz. Corrected replays use cutouts that
cover those complete trajectories. They still produce zero truth-near plots.

A migration-corrected complex trajectory search is rejected because the r277
positive control falls 0.20 dB below its control p99, showing that target phase is
not stable enough across CPIs. A local covariance-whitened spatial GLRT is more
promising as a future detector: with one fixed beam across the dwell it places
r277 11.38 dB above its control p99. However, after freezing its parameters on
r277 and applying exact signal-time truth, r12 is -1.43 dB, r94 -1.50 dB, and
r623 -2.10 dB. Their selected beams also vary substantially frame to frame. This
method retains the real positive but does not recover any miss, so it remains an
offline candidate and is not deployed.

The newest event's initial r620 capture was subsequently recovered from the live
node and evaluated separately from the later r623 periodic capture. Its complete
20-CPI truth trajectory overlaps the clip, but production replay still has zero
truth-near plots and the frozen fixed-beam spatial GLRT is 2.68 dB below its
matched-control p99, with 317 controls exceeding the target. The negative r623
result is therefore not an artifact of choosing the later periodic window.

## Post-Doppler multichannel STAP replay

The replay-only `analyze-post-doppler-stap.py` experiment jointly adapts the five
array channels across three adjacent post-Doppler filters (15 complex dimensions).
It uses robust energy clipping, diagonal loading, target-excluded local covariance,
one fixed beam across each exact signal-time trajectory, and the previously
measured +0.0258 km / +7.05 Hz replay calibration. The eight-point grid was frozen
on r277 before any miss was scored. Every empirical path repeats all 12 beam
trials; the reported threshold is the stricter of the matched-delay and complete
translated-path p99 values.

The positive r277 remains 6.819 dB above that threshold. None of the held-out
misses clears it: r12 is -3.771 dB, r94 -2.353 dB, r620 -2.189 dB, and r623
-4.589 dB. The complete accounting is retained in
`results/20261021-post-doppler-stap-matrix-v1.json`. This fails the predeclared
requirement to retain r277 and recover at least one miss by +3 dB, so the method
is rejected and was not deployed or connected to live processing.
