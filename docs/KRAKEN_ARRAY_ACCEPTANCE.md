# Kraken array geometry and channel-mapping acceptance

Status: design/audit contract plus a pure, non-integrated validator in
`api/kraken-array.js`, hardened after the independent Astra review. The module
records geometry only: `bearingImplemented`, `relativeBearingEligible`, and
`worldBearingEligible` are always false. Any future array-derived bearing remains experimental.
It must not be used by the tracker, detector, reference synthesizer, or any
model input. ADS-B remains display/evaluation truth only.

## Verified current behavior

This audit used VectorWarp commit `130d0e14345b4f7c903a21f1bdf4d2ae891e5320`,
the local KrakenSDR Suite V2 source at
`15c1a7ee3f05d909c0c5835addd126849a7d8deb`, and the official KrakenRF DoA
source at
[`2e1c4e6`](https://github.com/krakenrf/krakensdr_doa/tree/2e1c4e6a918f649f62c1b7a5c4c98a8b1bdc7e59).

VectorWarp currently accepts 2–8 coherent Kraken inputs. Its Heimdall decoder
preserves the wire order: channel 0's block, channel 1's block, and so on. It
checks the data-frame channel count and frequency against the VectorWarp
configuration. It receives no antenna position, serial, port label, or mounting
orientation in the 8091 data frame.

Heimdall assigns DAQ channel `N` to `expected_serials[N]`. The default serial
order is `1000` through `1007`; an operator may replace it with `--serials`.
Only the first active count is opened and streamed. USB enumeration order is
therefore not channel order. The 8092 status reports the active count and the
serial-list length, but not the serial values or physical antenna mapping.
Neither count is evidence of how antenna cables are attached.

Heimdall's compile-time `REF_CHANNEL` is the reference used for receiver
lag/phase compensation. VectorWarp's `capture.device.reference_channel` is the
direct-path passive-radar signal. These are different roles and must not be
silently equated.

VectorWarp has no DoA implementation today. Its detector and tracker contain
delay, Doppler, and SNR only. No bearing reaches track association. The separate
Suite V2 `kraken_doa_v2` client has UCA, ULA, and custom-position MUSIC controls,
but those controls and results are not Heimdall 8092 metadata and are not
consumed by VectorWarp.

The separate Suite MUSIC client uses all active channels, not an arbitrary
surveillance subset. Its current geometry controls cannot exclude a fifth
dedicated reference from a four-arm cross. That requires selected-IQ processing
and matching manifold order before any bearing capability can be enabled.

`array_eigenbeam` is not a direction finder. It estimates the principal
covariance eigenvector over the selected input channels and coherently combines
them into one reference signal. Selected synthesis channels may also be
surveillance channels. In `dedicated` mode, VectorWarp copies
`reference_channel` directly and requires that channel to be absent from
`surveillance_channels`.

## Concrete gaps

1. No current VectorWarp field records physical element identity, measured
   position, units, coordinate frame, mounting orientation, or the association
   between an element and a DAQ channel/receiver serial.
2. Heimdall status can confirm an active count, sample rate, frequency, and
   coherence state, but cannot confirm serial order or cabling. Its HTML page
   receives the startup serial list by template substitution; scraping that UI
   is not a control/status contract.
3. In dedicated mode the API does not require
   `process.reference_synthesis.channels` to equal `[reference_channel]`, even
   though the C++ processor ignores that list and uses `reference_channel`.
   The browser happens to normalize it, so hand-edited YAML can be misleading.
4. The shipped Kraken example is an all-channel `array_eigenbeam` configuration,
   not the confirmed four-surveillance-plus-dedicated-reference installation.
   It must not be relabelled as that installation without the operator's actual
   cable/channel mapping and measurements.
5. `Kraken` and `HeimdallFrame` contain one-channel lower-bound wording/checks,
   while the public configuration and Suite runtime require 2–8. Normal config
   prevents a one-channel run, but the internal contract is inconsistent.
6. Suite DoA geometry is private to the separate DoA client. A UCA control uses
   a radius in millimetres and assumes channel 0 at local +x with subsequent
   physical elements wired clockwise. A ULA control uses inter-element spacing
   in millimetres and retains an inherent front/back ambiguity unless an
   operator selects a half-plane. Custom positions are ordered by channel and
   expressed in millimetres. None of this is reported by Heimdall 8092.

## Proposed additive VectorWarp schema

Keep the existing channel-role fields authoritative for passive radar. Add an
optional, operator-owned `capture.device.array_geometry` object. Legacy Kraken
configurations without it continue processing but cannot claim a world-frame
bearing.

```yaml
capture:
  device:
    channel_count: 5
    reference_channel: 0
    surveillance_channels: [1, 2, 3, 4]
    array_geometry:
      shape: cross                 # cross | regular_polygon | ula | custom
      units: m                     # the only accepted VectorWarp unit
      coordinate_frame: array_xy_right_handed
      mapping_confirmed: false
      geometry_confirmed: false
      orientation_confirmed: false
      x_axis_bearing_deg_true: null # when confirmed: 0 north, 90 east
      bearing_channels: [1, 2, 3, 4]
      ula_half_plane: both         # both | positive_y | negative_y
      elements:
        - {id: reference, daq_channel: 0, receiver_serial: "1000", position: [REF_X, REF_Y, REF_Z]}
        - {id: plus-y,    daq_channel: 1, receiver_serial: "1001", position: [0, ARM, 0]}
        - {id: plus-x,    daq_channel: 2, receiver_serial: "1002", position: [ARM, 0, 0]}
        - {id: minus-y,   daq_channel: 3, receiver_serial: "1003", position: [0, -ARM, 0]}
        - {id: minus-x,   daq_channel: 4, receiver_serial: "1004", position: [-ARM, 0, 0]}
```

`REF_X`, `REF_Y`, `REF_Z`, and `ARM` above are deliberately symbolic. The UI
must require measured values and a physical cable walk; it must never ship
guessed site geometry or serials as an active bearing configuration.

The local frame is right-handed: +x and +y lie in the array plane and +z is up.
`x_axis_bearing_deg_true` is the clockwise-from-true-north bearing of local +x.
For a local mathematical azimuth `theta` measured counter-clockwise from +x,
the world compass bearing is `(x_axis_bearing_deg_true - theta) mod 360`.
This helper takes an unrotated local mathematical azimuth, not Suite's reported
or browser-displayed value. Suite ULA steering uses a broadside angle rather
than UCA/custom's +x mathematical angle; Suite adds `ARRAY_OFFSET` and its
compass display then reverses the angle. A future adapter must test these
conventions separately. No angle is currently computed or displayed by this
module, regardless of the orientation attestation.

Positions are canonical. `shape` selects validation/advice and never generates
or replaces positions. This prevents a count, a shape name, or a Suite config
echo from being mistaken for measured geometry.

`shape` classifies the geometry of `bearing_channels`; `elements` retains every
physical position regardless of role. Consequently, four vertices remaining
after dedicating one vertex of a physical pentagon are a `custom` bearing
manifold. Geometry does not assign or change reference/surveillance roles.
`mapping_confirmed`, `geometry_confirmed`, and `orientation_confirmed` are
separate operator attestations. They are reported as `operator-asserted`, without
verified provenance. Suite status can corroborate none of them.

## Pure validator result contract

`valid` refers to the structural geometry/role record, not receiver health,
current physical agreement, or the complete passive-radar configuration.
`state` is `configured-unverified` for a structurally valid record, `invalid`
for an invalid record, `unknown` for omitted Kraken geometry, and
`not-applicable` for another receiver. Missing geometry remains valid here and
does not prevent passive radar. An invalid optional bearing record must not be
mistaken for evidence that ordinary passive radar is impossible; full config
validation remains the config manager's responsibility.

`verification.channelCount.state` is `reported-match`, `reported-mismatch`,
`conflict`, `unavailable`, or `unreported`. These are observation labels only.
Raw/normalized contradictions are surfaced as conflicts; a count mismatch does
not change structural `valid`. Existing acquisition checks still reject an
incompatible actual stream. `verification.statusFreshness.state` and
`verification.physicalAgreement.state` are always `unverified`. This module
does not establish observation age, current serial order, or a surveyed mapping
bound to a particular acquisition. All bearing readiness remains false.

Counts and each channel/element list are capped at eight before traversal.
Coordinates must be complete three-value arrays, finite, and no greater in
magnitude than `Number.MAX_SAFE_INTEGER` metres to keep derived arithmetic
finite. Labels/serial metadata are trimmed strings of 1–128 characters without
control characters. Sparse and oversized arrays are rejected.

Measurement tolerances are validation policy, not calibration: absolute 1 mm;
cross midpoint/plane tolerance 2% of span and orthogonality normalized-dot
tolerance 2%; ULA collinearity 1% of span; ULA adjacent spacing 2% of mean gap;
polygon plane 2% of radius, radial tolerance 5%, angular-gap tolerance the larger
of 0.0873 radians or 5% of the ideal gap. Positional tolerances use the larger of
1 mm and the relative tolerance. Tilted crosses belong under `custom`.

## Shape and role acceptance

- Every active DAQ channel appears exactly once in `elements`; element IDs and
  DAQ channels are unique. `elements.length` equals `channel_count`.
- Positions contain three finite, bounded metre values. Receiver serial is optional
  operator metadata because current 8092 status cannot verify it.
- `bearing_channels` is unique, in range, and a subset of
  `surveillance_channels`. In dedicated mode it excludes `reference_channel`.
- The confirmed cross has exactly four bearing channels at the four arms of a
  measured plus, and one fifth element used only as the dedicated reference.
  The reference position is recorded but must be excluded from any future DoA
  covariance. No such selected-channel covariance is implemented yet.
- Five physical positions may form a pentagon while one vertex is the dedicated
  reference and the other four form an irregular bearing subset. That valid
  four-element subset is recorded as `custom`, not silently promoted to a
  five-bearing regular polygon. Five bearing elements plus a separate dedicated
  reference require at least six streamed channels. Do not treat the
  cross-plus-reference and pentagon as equivalent layouts.
- A regular polygon supports 3–8 bearing elements with explicit positions. Its
  measured channel order need not match Suite's UCA shorthand; custom-position
  transfer is required when it does not.
- A ULA supports 2–8 explicitly positioned bearing elements. Validation checks
  collinearity and equal adjacent spacing within a documented measurement
  tolerance. `both` is the safe default because a ULA cannot resolve front from
  back without independent operator knowledge. ADS-B must not choose a lobe.
- `custom` accepts nonuniform and irregular 2-D/3-D positions without claiming
  UCA/ULA symmetry. Unknown or incomplete geometry blocks bearing, not ordinary
  passive-radar processing.
- Collinear horizontal apertures retain mirror ambiguity regardless of shape
  label. A vertical-only aperture provides no azimuth information. Diagnostics
  report these limitations without claiming a physically calibrated manifold.
- Wavelength/spacing checks are warnings, not fabricated calibration. Official
  KrakenRF guidance recommends keeping inter-element spacing below 0.5
  wavelength to avoid ambiguous bearings, but arbitrary arrays require a real
  manifold/aliasing assessment rather than one universal distance rule.
  `spacingAssessment()` reports the checked method and threshold separately.
  `aliasRisk` is `true` when a checked spacing reaches half a wavelength, and
  `null` otherwise; it never reports an arbitrary manifold as alias-free.

## Field agreement matrix

| Concern | VectorWarp today | Heimdall 8091/8092 | Suite `kraken_doa_v2` | Required authority |
|---|---|---|---|---|
| Active channels | Configured `channel_count`, 2–8 | Data header and status `num_channels`; status `max_elements` is serial-list length | Follows data header, 2–8 ceiling | Heimdall count; mismatch is a hard stop |
| Channel order | Positional `0..N-1` | Channel N is `expected_serials[N]`; serial values absent from 8092 | Uses first N channels positionally | Suite startup plus operator cable map |
| RF/sample rate | Config and frame frequency check; Kraken sample rate fixed 2.4 MS/s | Status reports both | Uses packet metadata | Heimdall readback |
| Heimdall calibration reference | Not represented | Compile-time `REF_CHANNEL` (currently 0), absent from 8092 | Receives already compensated IQ | Heimdall implementation; do not mirror into radar role |
| Passive-radar reference | `reference_channel` | Not known | Not known | VectorWarp config |
| Surveillance roles | `surveillance_channels` | Not known | Not known | VectorWarp config |
| Reference synthesis | `dedicated` or covariance `array_eigenbeam` | Not known | Separate beamforming/DoA concepts | VectorWarp config |
| Physical positions | Absent | Absent | UCA/ULA shorthand or custom mm positions | Operator measurement |
| Shape/spacing | Absent | Absent | UCA radius mm; ULA spacing mm; custom positions mm | Operator config; no implicit sync |
| Orientation | Absent | Absent | Array offset rotates its own output; true-north provenance not reported | Operator survey |
| Bearing | Absent | Absent | MUSIC output from separate client | Experimental display only |
| Tracker input | Delay, Doppler, SNR | N/A | N/A | Bearing and ADS-B prohibited |

## Regression gates for implementation

1. Cross fixture: arbitrary DAQ permutation maps four surveillance elements
   and one dedicated reference; the reference is excluded from bearing and
   surveillance, while `process.reference_synthesis.channels` is exactly the
   one reference channel.
2. Pentagon fixtures retain every explicit channel mapping: five physical
   vertices with one reference accept a four-bearing `custom` subset, while
   five-bearing-plus-reference fails at count five and passes at count six.
3. ULA fixtures: 2–8 elements pass only with unique, in-range mappings and
   explicit positions; front/back defaults to `both`; a selected half-plane is
   labelled operator-constrained, never truth-derived.
4. Custom fixtures retain irregular positions and reject NaN, infinity,
   duplicate channels/IDs, missing channels, and out-of-range channels.
5. Status reconciliation reports observations only; stale, unavailable,
   contradictory, or count-only status cannot enable bearing. Missing
   serial/geometry/freshness evidence remains `unverified`.
6. Static boundary test proves Detection/Tracker schemas and tracker calls have
   no bearing/azimuth/ADS-B input.
7. Any future Suite geometry transfer must be explicit, unit-converted, and
   acknowledged by the actual DoA service. Heimdall config/status echo is not
   physical calibration.
8. Run both `api/kraken-array.test.js` and
   `api/kraken-array-adversarial.test.js`. The latter includes status conflicts,
   input bounds, nonuniform ULA rejection, vertical and collinear ambiguity,
   and an exactly aliased rectangle missed by a nearest-neighbor-only rule.
   Passing these pure tests does not complete config/API/browser integration.
