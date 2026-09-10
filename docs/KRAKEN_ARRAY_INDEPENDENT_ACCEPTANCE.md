# Independent Kraken physical/software agreement review — 2026-09-10

Decision: the initial pure geometry module is NOT accepted for integration as
bearing-ready. The existing passive-radar role separation is supported by source
inspection. Geometry recording can proceed as operator-owned metadata once the
configuration/browser path is integrated and validated. Actual bearing readiness
must remain false: VectorWarp has no bearing implementation, and the separate
Suite MUSIC client cannot select the required surveillance-only subset.

Review requested with GPT-6 Astra at ULTRA effort. No service, hardware, Pi,
compute limit, lock, deployment, or source-control mutation was performed.
The initial review added this report and `api/kraken-array-adversarial.test.js`.
The subsequent authorized remediation changed only the pure module, its two
test files, and these acceptance documents. No config/UI integration or Suite
change was made by this review workstream.

## Module remediation outcome

The pure-module blockers identified below are remediated. Both bearing
eligibility flags and `bearingImplemented` are unconditionally false on every
return path. Structural record validity is separate from reported count
agreement. Count states use `reported-match`/`reported-mismatch` or explicit
conflict/unavailability, never physical verification. Operator booleans are
`operator-asserted`; physical agreement and status freshness stay unverified.

Counts/list sizes are bounded before iteration or allocation, sparse arrays
are rejected, and coordinate/label limits keep arithmetic and output bounded.
ULA validation checks equal projected gaps. Collinear/vertical apertures and
tilted cross shorthand are diagnosed. Spacing results preserve possible or
unresolved aliasing instead of false assurance from small nearest-neighbor
gaps. Full documentation of the return contract and tolerances is in
`KRAKEN_ARRAY_ACCEPTANCE.md`.

The evidence hashes and findings below describe the original reviewed input;
they are retained as the reproducible reason for remediation. Current module
SHA-256: `9d2a0b19a98d67eb23a60decf4f4f172a2056491c0b5f9a7997a9ac1850fa0d4`.
The actual config/API/browser integration, physical survey provenance, selected
IQ path, and Suite coordinate adapter remain deferred acceptance gates.

## Evidence scope

VectorWarp working source: `/opt/blah2-kraken-2to8-ui-config`, HEAD
`aaf34a2143f52f8970c890973264679555043573`, plus uncommitted files. The original
audit document names an older commit; this review inspected the actual files.
Suite source: `/opt/krakensdr_suite_v2`, HEAD
`15c1a7ee3f05d909c0c5835addd126849a7d8deb`, with existing changes in the TCP
control/data and SDR pipeline files. A commit ID alone does not pin that tree.

Reviewed source SHA-256:

| File | SHA-256 |
|---|---|
| VectorWarp `api/kraken-array.js` | `f1398dbd13cccb788e80a31098910c6502a4af089be31e993f07d581fd49ba2e` |
| VectorWarp `api/kraken-array.test.js` | `6264bb874902c79207000936bf63b90529046e59c3599c52002997d2ac80558a` |
| VectorWarp `api/upstream-status.js` | `25a021bd7469438e6e84b5aeb4b56d6a87c7276a4366890165affdb301a91e24` |
| VectorWarp `html/js/config_ui.js` | `a3ef7633ec6aefa9473a808c4eb33c69c8252ba91f71a533b705ec0f9f35f534` |
| VectorWarp `src/blah2.cpp` | `1f0529a86181fd0b5dec1156b9f5261b3bda78e75ac437347114513d64203ecf` |
| Suite `heimdall_v2/src/net/tcp_control_server.cpp` | `4357f128dc581c97634cca080bb1a86744082e437f1d4097b3bd998db399c806` |
| Suite `heimdall_v2/src/main.cpp` | `4dd6cc88c9f200515f3184a11b1bb114c2b87991a4aa85dc518a89cda6adf853` |
| Suite `heimdall_v2/src/sdr/sdr_init.cpp` | `a2e01c1054a65858a65b39226a76109a688f892012f83476ea45eb704b5315e3` |
| Suite `kraken_doa_v2/src/signal_processing/music_processor.cpp` | `9367ddb870a032dded7c33c61e92ebc2c583aac80de8bb78bed44f1bfd1696bc` |

Read-only systemd observation: `krakensdr-suite-v2.service` was inactive/dead.
Its configured command was `heimdall --num-elements 5 --serials
1000,1001,1002,1003,1004`. This is intended startup configuration, not evidence
that five devices are currently open, that these serials were successfully
opened by this binary, or that any cable reaches a particular antenna. No live
8091/8092 agreement or physical installation is claimed.

## Original findings requiring correction or explicit deferral

1. **Readiness overclaim.** `api/kraken-array.js:268` accepts a matching integer
   count from raw or normalized status. Lines 292–306 make both eligibility
   flags true from that count and three operator booleans. Availability,
   `checkedAt`, RF/sample-rate conflicts, operating mode, calibration, recovery,
   and reconfiguration are ignored. `{num_channels:5}` suffices, as do stale or
   explicitly unavailable normalized observations. Contradictory raw/normalized
   counts prefer the raw match. Keep actual readiness false in this release;
   expose separate structural validity, operator attestations, and observed
   upstream agreement. Future live eligibility needs fresh, endpoint/config/
   acquisition-bound evidence and fresh synchronized input, not cached counts.

2. **No dedicated-reference exclusion in Suite MUSIC.**
   `music_processor.cpp:127` synchronizes to the active packet channel count;
   lines 279–295 copy channels `0..N-1` into covariance input. The inspected DoA
   command contract has geometry settings but no surveillance/bearing subset.
   `CUSTOM_POSITIONS` supplies positions for all active channels in order.
   Four '+' surveillance elements plus a fifth dedicated reference therefore
   cannot be implemented by geometry transfer alone. Lowering the active count
   to four selects the first four devices and changes acquisition; it cannot
   select an arbitrary four-channel permutation. Required future work is an
   explicit selected-IQ path preserving DAQ identities, covariance order, and
   matching steering-vector order. This is outside a metadata-only release.

3. **Coordinates differ by Suite topology and display.** Suite UCA/custom
   steering uses `exp(+j*2*pi*(x*cos(theta)+y*sin(theta))/lambda)`;
   ULA uses `exp(+j*2*pi*x*sin(theta)/lambda)` at lines 1086–1100. Thus its ULA
   angle is a broadside convention, not the same +x mathematical azimuth.
   `applyOutputTransforms()` adds `ARRAY_OFFSET` before publishing. The actual
   browser's `displayAngle()` (`kraken_doa.html:1894`) then returns
   `(360-azimuth) mod 360` in compass mode. Applying the draft helper to an
   already rotated/display-converted value would double-transform it.
   For UCA/custom raw angle theta, desired VectorWarp bearing is `B_x-theta`;
   Suite compass display is `-(theta+offset)`. Algebraically, these agree when
   `offset=-B_x` for identical axes, before discretization. This relation is
   NOT a tested adapter and does not cover ULA. Half-plane correspondence also
   needs the ULA axis and topology convention; `positive_y` cannot be blindly
   translated to Suite `FORWARD` for arbitrary rotated linear positions.

4. **ULA validation contradicts its contract.** Lines 244–258 check
   collinearity but not equal adjacent projected spacing. Positions
   `x=[0,.2,.6,1.4]` are accepted as a ULA. Such geometry is valid as `custom`,
   but cannot be represented by Suite's single `SPACING` value. Tolerances
   must be documented and applied to the sorted projected gaps. Also diagnose
   the measured manifold regardless of shape label: a custom collinear array
   still has mirror ambiguity; a purely vertical aperture has no azimuth
   information. Current tests reproduce both missed conditions.

5. **Nearest-neighbor distance cannot clear aliasing.** At wavelength L, the
   custom rectangle `(0,0),(L,0),(0,.1L),(L,.1L)` has nearest-neighbor gaps
   `.1L`, so `spacingAssessment()` returns `aliasRisk:false`. Yet the two unit
   directions `(.5,sqrt(.75),0)` and `(-.5,sqrt(.75),0)` have identical steering
   vectors: their phase difference at each element is 0 or 2*pi. This is an
   exact counterexample, not a statistical possibility. Keep the simple
   spacing check explicitly heuristic and represent arbitrary-manifold aliasing
   as unresolved unless actually assessed. A shape label must not hide
   ambiguity. ULA spacing >= L/2 also needs care at the visible-sector endpoints.

6. **Invalid counts can throw before returning validation.** An integer count
   `2**32` records a range issue but later reaches
   `Array.from({length:count})` at line 228, throwing `Invalid array length`.
   Bound the count before any allocation or derived loop. Configuration errors
   must remain bounded structured errors.

7. **The proposed field still has no real config/UI path.**
   `api/config-manager.js:199` rejects device fields absent from its selected
   profile; `array_geometry` is absent. The browser has no geometry editor or
   agreement view; `normalizeKrakenChannels()` changes roles/counts without a
   geometry-attestation invalidation contract. The pure module is not imported
   by server, config manager, or browser. `api/package.json` does not run its
   tests. Integration must add optional persistence and reload coverage, handle
   count/role/permutation edits, and prevent canned survey/serial confirmations.

8. **Confirmations lack provenance.** Element serial strings remain operator
   metadata, which is honest, but `mapping_confirmed:true` is not bound to an
   ordered Suite startup serial list, endpoint/acquisition generation, or config
   digest. A same-count serial-order restart or a cable swap can silently retain
   all confirmations. Bind records to the relevant configuration and require
   an explicit cable survey; invalidate affected attestations on edits or known
   acquisition changes. When runtime serial order is not reported, show that
   missing evidence and keep runtime physical agreement unverified.

## Required physical/software evidence matrix

| Concern | Physical/operator evidence | Suite evidence | VectorWarp agreement and acceptance |
|---|---|---|---|
| Channel identity/order | Cable walk: antenna ID -> connector -> receiver serial; record date and scope | `expected_serials[N]` and successfully opened serial for DAQ N; USB enumeration index is transient | Explicit DAQ N -> element row; same active count alone is insufficient |
| Active count | Number of connected intended coherent elements | 8091 count, fresh 8092 `num_channels`; `max_elements` is serial-list length | 2–8, same count as config; conflict stops compatible processing/bearing; absent geometry alone does not |
| Calibration reference | Which receiver participates in coherent calibration, separate from signal role | Compile-time `REF_CHANNEL=0`; lag/phase correlation and compensation use it | Never infer radar reference from it or reorder antennas to make the two roles coincide |
| Passive-radar roles | Direct-path reference antenna and each surveillance antenna identified | Heimdall does not know these roles | `reference_channel`, `surveillance_channels`, synthesis mode; dedicated reference excluded from surveillance and bearing |
| Physical positions | Measured x/y/z, units, origin, uncertainty, phase-center/cable assumptions | UCA radius mm; ULA spacing mm; custom x/y/z mm in active order, separate DoA service | Preserve all measured elements in metres; `shape` classifies bearing subset, not every physical element |
| Geometry transfer | Actual measured array and cable order | UCA assumes ANT0 on +x, following channels clockwise; custom values are literal | UCA transfer only if exact convention/order fits; otherwise explicit custom mapping, with tested 1000 mm/m conversion |
| Orientation | Surveyed true bearing of +x; +z up; true/magnetic provenance | Added `ARRAY_OFFSET`; browser reverses angle; ULA uses a distinct broadside convention | For local CCW azimuth, `bearing=(B_x-theta) mod360`; unknown survey permits only clearly relative metadata/angle |
| Coherence/freshness | Coherent hardware/clock and calibration procedure | 8091 phase/noise/retune flags; 8092 recovering/cooldown/reconfiguring and limited calibration data | Fresh synchronized input required; status echo/compensation vector alone does not prove physical phase calibration |
| Actual bearing inputs | Confirmed physical surveillance subset | Existing MUSIC consumes all N channels | Unsupported until selected-IQ and matching manifold are implemented; dedicated reference must never enter covariance |
| Truth and tracker | Independent evaluation/display provenance | No ADS-B role in this contract | Bearing experimental; no detector, tracker, reference-synthesis or model input; ADS-B cannot choose a lobe |

Heimdall source confirms serial-based matching in `sdr_init.cpp:43`; it
re-enumerates on each open. Startup serial length, USB presence, and the HTML
template's serial array are different evidence from successful runtime opens.
The current opening code only warns if a serial differs during the later open
check (`sdr_init.cpp:123`), and CLI parsing validates list length rather than
uniqueness. Do not imply that those source paths establish an immutable physical
mapping. An explicit mismatch is a conflict, not a reason to relabel the survey.

The draft uses cardinal element names before the orientation is known. Prefer
local names such as `plus-x` and `plus-y` until surveyed: +x east implies +y
north; +x north implies +y west in a right-handed frame with +z up.

The official [KrakenRF quickstart](https://github.com/krakenrf/krakensdr_docs/wiki/02.-Direction-Finding-Quickstart-Guide)
also requires correct antenna-to-channel wiring and matching antenna/cable
hardware. These physical conditions cannot be inferred from a status echo.

## Concrete layout acceptance

| Requested layout | Passive-radar/geometry contract | Bearing interpretation |
|---|---|---|
| Four surveillance '+' arms plus fifth dedicated reference | Five total; arbitrary explicit DAQ permutation; exactly one dedicated reference and four surveillance/bearing channels; record reference position independently | Four-element measured cross; reference excluded from covariance; currently unsupported at runtime |
| Five physical pentagon vertices, one dedicated reference | Five total; retain all five physical positions; four surveillance/bearing channels | Four-vertex custom subset of the pentagon, not a five-bearing UCA |
| Five bearing pentagon vertices plus separate dedicated reference | At least six total; five surveillance/bearing channels and separate reference | Explicit five-element regular-polygon manifold, subject to selection/order support |
| Five physical pentagon vertices with array-eigenbeam reference | Five total; reference synthesis may use same IQ channels as surveillance | Five bearing inputs can be structurally recorded; eigenbeam is covariance-based reference synthesis, not DoA |
| ULA across the 2–8 receiver range | Explicit positions; unique channels; equal projected gaps for `ula`; allow nonuniform data as `custom` | Front/back remains visible unless separately operator-constrained; rotated axis and spatial aliases remain explicit |
| Two total channels with dedicated reference | One reference and one surveillance path remains valid passive radar | Only one bearing element: insufficient for array bearing; this must not block passive-radar configuration |
| Unknown geometry, stale/missing evidence, or unconfirmed survey | Keep valid ordinary passive-radar config; surface exact missing evidence | Never verified or ready for bearing |

## Browser release gates and minimum implementation

1. Provide an optional geometry section with a measured element table, explicit
   DAQ/serial association, units, role columns, separate bearing selection, and
   orientation survey. A cross or polygon preset may guide entry but must not
   silently assert measured site positions, cable mapping, or serials.
2. Save/reload preserves operator values and roles. Legacy unknown geometry
   remains usable for passive radar. Incomplete bearing records remain clearly
   incomplete; a one-surveillance dedicated setup does not inherit a two-bearing
   prerequisite for passive operation.
3. Count, reference, surveillance, bearing order, serial mapping, position, and
   orientation edits invalidate the corresponding confirmations. Switching from
   eigenbeam to dedicated requires a selected real reference and excludes it.
   No count decrease/reincrease resurrects old confirmations without review.
4. Show separate configuration validity, operator survey state, fresh receiver
   status, unreported serial/geometry evidence, and actual implementation
   capability. Use “operator-recorded; physical agreement unverified” and
   “bearing unavailable/experimental” as appropriate; never a generic green
   “verified” result based on counts or configuration echoes.
5. Actual bearing readiness remains false in this release. Before lifting it,
   implement selected-IQ extraction, per-channel manifold order, and a tested
   Suite output adapter. Tests must include every cardinal direction, oblique
   angles, sign, chirality, rotation, ULA broadside/half-plane mapping, and mm/m
   conversion. Inject recognizable channel data so including the dedicated
   reference or applying a DAQ permutation incorrectly fails deterministically.
   Synthetic tests verify software conventions; a separate physical known-angle
   survey is still necessary to rule out wiring/phase/sign errors.
6. Reset bearing snapshots and pending output on count/order/frequency/config/
   acquisition changes. Reject delayed responses from the old context. Suite
   may hold an old DoA result during squelch or calibration, so output freshness
   needs its own evidence beyond an available receiver status.
7. Keep the tracker/detector/model/truth boundary regression gates. None of the
   new metadata, candidate angles, truth labels, or half-plane choices enters
   radar inference, association, or reference synthesis.

## Validation and next acceptance check

Existing capped container verified before use: `blah2-ui-config-preview`,
source read-only at `/src`, 0.5 CPU, CPUs 8/24, 8 GiB RAM and no swap allowance.
No new container or hardware/GPU test was run.

Initially, `node api/kraken-array.test.js` passed while the independent suite
produced **7/22 passes and 15 acceptance failures** against the original hashes
above. Those failures were not accepted expected behavior.

After module remediation, the original suite and all **32/32** expanded
independent acceptance cases pass in the same capped container. This includes
healthy, stale, absent, unavailable, transitional, mismatched, and conflicting
status without any route to bearing readiness. Syntax and `git diff --check`
checks pass. No hardware or integration acceptance is claimed.

Next acceptance check: integrate and review the real config/API/browser
save-reload flow against the matrix above. Passing this metadata gate does not authorize actual bearing,
hardware promotion, deployment, or changing the physical installation.
