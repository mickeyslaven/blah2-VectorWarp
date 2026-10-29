# Tracker false-track audit — 2026-10-28

## Verified production state

- The live publication mode was `native`, not `hybrid` or `external`.
- The old external/Retina event files stopped advancing on 2026-10-24 and the
  external tracker service was inactive. It therefore could not be the source
  of the current false tracks.
- Native `ASSOCIATED` is a pre-confirmation M-of-N state. The web Map and radar
  overlays incorrectly presented it as an operational track. Operational views
  now show only confirmed `ACTIVE` tracks, with previously confirmed
  `COASTING` tracks kept visually distinct. Candidate states remain available
  on the Tracks diagnostic page.
- A sampled live frame contained 40 tentative, 10 associated candidates,
  zero active tracks and eight coasting tracks. The large visible population
  was therefore mostly candidates, not confirmed aircraft tracks.

## Default profiles

Fresh installations use `process.tracker.profile: conservative`:

- 3 detections in a 6-CPI confirmation window;
- a 7 dB minimum SNR for **starting** a track;
- a weak detection may still update an already established track;
- three missed CPIs retire a track by the fresh-install defaults;
- the GPU-resident persistent stationary-clutter initiation filter remains an
  independent setting and is not disabled by either profile.

`process.tracker.profile: permissive` preserves the released initiation
behavior by disabling the initiation-SNR gate. Existing configurations that
do not contain a profile remain permissive; merely opening and saving an
unrelated setting cannot silently migrate them.

The profile does not replace the association algorithm. VectorWarp still uses
its native resident tracker; “conservative” adopts Retina-like admission and
confirmation policy without depending on an external process or event file.

## Stationary clutter and shadow separation

- Main clutter cancellation (`process.clutter`) is live when enabled.
- GPU-resident persistent stationary clutter is a live initiation filter. It
  learns recurring delay-Doppler cells and prevents only new tracks from
  starting there. It does not remove the detector cell or block an existing
  moving track that crosses zero Doppler.
- Track-guided recovery with `promote: false` is shadow-only.
- Bearing/detection manifold candidates remain shadow-only until explicitly
  qualified and promoted.

## Acceptance boundaries

Unit and wire tests verify that the CPU and GPU paths apply the same minimum
initiation threshold and that it is bound into resident configuration identity.
The resident wire schema is bumped so an old worker cannot silently interpret
the newly occupied field. These tests establish implementation integrity, not
aircraft recovery. Live acceptance must compare confirmed-track and false-track
burden over matched traffic while preserving RF, gain, geometry and detector
settings.
