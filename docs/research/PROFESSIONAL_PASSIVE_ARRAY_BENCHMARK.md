# Professional single-site passive-radar arrays: evidence review and VectorWarp roadmap

**Status:** read-only engineering research; no production code, receiver setting, live configuration, or promotion state was changed.

**Repository audited:** `detection-recovery` at `46b4c61934deea2e386049743293be995a872170` on 2026-10-22.

**Site basis supplied by the user:** five coherent KrakenSDR channels; five nominally vertical monopoles on a regular pentagon of approximately 148 mm radius; operation near 550 MHz (historical evidence also includes 527 and 539 MHz); 2.4 MS/s; approximately 200 ms CPI and historically approximately 10 output updates/s; a roof-edge installation with strong direct-path and indoor-to-outdoor/near-building multipath; eventual inference must not depend on ADS-B.

This report is intentionally conservative. It separates four evidence classes:

- **Confirmed public fact:** stated in a manufacturer, government, university repository, paper, thesis, patent, or directly inspected source code.
- **Supplier claim:** a performance/capability statement from sales material without enough public test detail to reproduce it.
- **Not publicly established:** a detail not found in the accessible sources. It must not be reverse-engineered from product photographs.
- **Engineering interpretation / hypothesis:** an inference offered for testing, not a measured VectorWarp result.

The companion [evidence ledger](PASSIVE_ARRAY_EVIDENCE_LEDGER.md) gives dates, URLs, source type, and confidence. Links to figures and product photos are links to their original publishers; no classified or visually inferred architecture is asserted.

## Executive finding

The professional pattern is not “pick a clever array shape and run MUSIC.” It is a system discipline:

1. preserve analog headroom before the ADC;
2. digitize each element coherently;
3. calibrate the complete installed receive manifold, not only cable phase;
4. form/reference/null signals spatially and cancel direct-path plus multipath digitally;
5. retain ambiguity hypotheses when the aperture cannot resolve them; and
6. validate on independent target passes and configurations.

VectorWarp already implements substantial parts of that pattern: coherent five-channel intake, array-eigenbeam reference synthesis, coherent and noise-whitened fusion, a bounded direct-path null, candidate-local covariance MVDR, measured-manifold recording and pass-balanced held-out promotion, raw headroom telemetry, and a frequency-resolved multipath shadow. Those capabilities are **present**, usually opt-in, and must not be described as missing. The dominant gap is evidence: the current installed array does not yet have a clean, independently validated direction-dependent manifold, and the site photographs plus the Ford Street survey show an environment in which a free-space pentagon model is unlikely to be sufficient.

The highest-value next step is therefore a controlled **site/manifold and headroom campaign**, not a new super-resolution algorithm. If that campaign fails, moving the aperture into clearer space is more likely to help than adding more adaptive DSP to contaminated five-channel data.

## 1. Public system comparison

### 1.1 Systems and what the public record actually supports

| System | Publicly supported receive/array facts | Publicly supported processing or results | Claims that remain claims | Not publicly established |
|---|---|---|---|---|
| **HENSOLDT TwInvis** | Exploits FM, DAB and DVB-T; official material shows a stationary mast installation and states 360° azimuth and 3D tracking. Multiple illuminators and clustered sensors are supported. [Official page and photo](https://www.hensoldt.net/products/twinvis-passive-radar-surveillance-of-noiseless-objects), [2018 data sheet](https://www.hensoldt.net/fileadmin/HENSOLDT_2019/Products/Radar_IFF_Datalink/0570_18_TwInvis_Passive_Radar_datasheet_E_preview.pdf). | Official figures state real-time fusion of 16 FM transmitters and five DAB/DVB-T networks, sub-second updates in current web material, and sensor clustering. | Detection of stealth aircraft, ranges to 250 km on FM and approximately 100 km on DAB/DVB-T, and advertised accuracy are supplier claims without a public reproducible trial protocol in the cited material. | Element count, array radius, element pattern, RF/ADC dynamic range, calibration/manifold method, pre-ADC cancellation, beamformer/DOA algorithm, and deployed covariance processing. The product photograph is not evidence of these details. |
| **Silentium Defence MAVERICK / OMNIGUARD** | M8 is described as a portable near-360° product; Excalibur as relocatable 3D; OMNIGUARD as long-duration space surveillance. An AMOS paper states that an independently digitized surveillance array forms simultaneous receive beams and uses multiple FM transmitters up to 400 km from the receiver. [Product family](https://silentiumdefense.com/technology/), [AMOS abstract](https://amostech.space/year/2024/passive-radar-for-launch-and-re-entry-support/). | The AMOS contribution is technical evidence for element-level digitization and digital multi-beam operation at the Oculus/MAVERICK-S site. A 2015 paper by Searle, Davis and Palmer studies extracting reference and surveillance information from a single received signal. | Deployment time, coverage language, survivability and tracking quality on the product pages are supplier claims. | M8/Excalibur/OMNIGUARD element counts, exact geometries, calibration, ADC depth, analog cancellation, and operational DOA algorithms. Product imagery is not a reliable architecture diagram. |
| **IAI/ELTA ELK-7080 PCL** | Official page says FM/DAB, one antenna site or a network, 360° 3D real-time coverage, multi-target tracking, and central C2 connection. [Official product page and gallery](https://www.iai.co.il/product/elk-7080-pcl-passive-coherent-locator-system/). | Networked and standalone operating concepts are public. | “Immune to jamming,” accuracy, and operational performance are not accompanied by public measurement detail on the page. | Element-level architecture, array geometry, reference-channel arrangement, calibration, dynamic range, cancellation, and DOA estimator. |
| **BATS PCL** | BATS states FM/DAB, a single site or network, 360° 3D real-time coverage and central C2. [Official BATS page](https://www.bats.be/passive-coherent-location-system/). | Public architecture is site/network level only. | Accuracy and target capacity are not reproducibly quantified. | The public wording is very similar to IAI's page, but the cited pages do not establish an OEM, licensing, or technical identity. Array geometry and algorithms are undisclosed. |
| **Warsaw University of Technology FM PCL** | The 2008 IEEE paper explicitly describes **eight half-wave dipoles in a circular array**, 360° multiple beams, mutual-coupling correction, antenna-pattern optimization, field calibration and measurements. [IEEE record](https://doi.org/10.1109/RADAR.2008.4720988). | Demonstrated digital beamforming with full-system field calibration. It is direct evidence that practical circular arrays need coupling/front-end correction rather than geometry-only steering. | None beyond what the paper reports. | The accessible abstract does not expose enough detail here to transfer its exact dimensions or coefficients to VectorWarp. |
| **TNO FM PCL** | The 2012 paper describes an **eight-element circular array**, simulation plus measurement, digital compensation of mutual coupling/channel effects, sidelobe-reduced DBF, direct-path suppression, range-Doppler processing, GO-CFAR and ADS-B comparison. [TNO repository](https://repository.tno.nl/SingleDoc?docId=34257), DOI [10.1049/iet-rsn.2011.0401](https://doi.org/10.1049/iet-rsn.2011.0401). | Experimental validation and comparison with ADS-B are confirmed. A related paper compares S-parameter/front-end calibration with a total coupling matrix measured from known source positions, and reports front-end phase shifts as the larger error for its array. [TU Delft record](https://research.tudelft.nl/en/publications/mutual-coupling-compensation-applied-to-a-uniform-circular-array/). | None beyond the published experiment. | The paper does not prove that mutual coupling is negligible for VectorWarp's different elements, spacing and roof environment. |
| **Leonardo/SELEX AULOS** | Published engineering description identifies direct RF sampling, high-dynamic-range digitizers, FPGA DDC, synchronized multichannel PXI acquisition and a planned dual-band mobile design using two UCAs of half-wave dipoles. [2013 system article with figures 2 and 5](https://www.microwavejournal.com/articles/18896-aulos-a-passive-covert-radar-system), [official data sheet](https://electronics.leonardo.com/documents/16277707/18362811/Aulos.pdf?t=1669993679223). | Later work reports robust disturbance cancellation and joint use of multiple FM channels, tested against traffic of opportunity and cooperative small targets. [2018 paper record](https://doi.org/10.23919/IRS.2018.8448025). | Several-hundred-kilometre monitoring and small-RCS language is vendor material unless tied to a disclosed trial. | Production antenna counts, exact manifold calibration, analog suppression and current product algorithms. The 2013 “future” UCA design must not be rewritten as a confirmed current configuration. |
| **Lockheed Martin Silent Sentry 2** | A 1999 Lockheed briefing explicitly states a **horizontal linear phased target array**, separate single-element reference antennas, high-dynamic-range receivers and 90° staring coverage. [Briefing PDF, pp. 3–5](https://jedi-sec.us/downloads/Dept_Of_Defense/dod-lockheed_martin-passive-radar-salespitch.pdf). | Delay/Doppler extraction and state tracking across multiple FM illuminators are described; 2-D/3-D solutions depend on geometrically diverse illuminators. | Operational range and broad threat coverage in a sales briefing are not an independent acceptance test. | Modern/current implementation, calibration coefficients, ADC depth and cancellation details. |
| **16-element AM-radio passive OTH radar demonstrator** | Open paper reports a 38 m diameter UCA of sixteen 10 m monopoles, array calibration, direct-path DOA/reference beamforming, channel-wise direct-signal cancellation, coherent processing, azimuth-Doppler maps and track-before-detect. [Open paper and figures](https://doi.org/10.1155/2021/3911956). | A 118 s field data set used 4 s CPIs with 3 s overlap and ADS-B for eight civil aircraft; the paper explicitly fixes elevation because the UCA's elevation resolution is low. | It is a research demonstrator at HF, not a direct performance template for UHF VectorWarp. | Transferability of its long-CPI algorithms to a five-channel, 550 MHz, roof-mounted aperture. |
| **ICRAR/Curtin portable SDA array** | University page reports 32 antennas in a 35 m circular array for passive space/aircraft sensing. [ICRAR page and array photograph](https://www.icrar.org/case-studies/space-domain-awareness/). | Confirms the professional/scientific use of large many-element circular apertures for sensitivity and angular information. | The outreach page is not an algorithm or sensitivity specification. | Direct relevance to a five-element small-aperture local air-surveillance receiver. |

### 1.2 Cross-system lessons

- **Circular arrays dominate when one fixed site must stare over 360°.** Warsaw, TNO, the AM-radio demonstrator and ICRAR provide public examples. The benefit is azimuthal symmetry, not magical resolution.
- **Linear arrays remain attractive for a bounded sector.** Silent Sentry's disclosed horizontal linear array covers 90° and uses separate references. A ULA can support simpler phase-mode/root methods and spatial smoothing, but it has front/back ambiguity unless element patterns, multiple faces or other information resolve it.
- **Professional systems separate functions when hardware permits.** Silent Sentry discloses dedicated reference antennas. AULOS describes multichannel acquisition and high dynamic range. Public product pages for TwInvis, MAVERICK, ELK-7080 and BATS do not reveal whether or how this is done.
- **Installed calibration is a first-class subsystem.** Warsaw and TNO measured the complete array/front-end response; TNO's comparison warns that front-end phase can dominate coupling.
- **Multiple illuminators and/or multiple receiver sites create geometry that one small array cannot.** Every product family advertises multi-illuminator or network operation. That is distinct from increasing the element count of one local aperture.

### 1.3 Figure and photograph index

These are visual context only; accompanying text, not appearance, is the basis for technical claims.

- HENSOLDT's [TwInvis page](https://www.hensoldt.net/products/twinvis-passive-radar-surveillance-of-noiseless-objects) and [data-sheet “typical antenna” figure](https://www.hensoldt.net/fileadmin/HENSOLDT_2019/Products/Radar_IFF_Datalink/0570_18_TwInvis_Passive_Radar_datasheet_E_preview.pdf).
- IAI's [ELK-7080 official gallery](https://www.iai.co.il/product/elk-7080-pcl-passive-coherent-locator-system/).
- Leonardo/SELEX AULOS [mobile system photograph and receiver architecture, figures 2 and 5](https://www.microwavejournal.com/articles/18896-aulos-a-passive-covert-radar-system).
- Lockheed Martin Silent Sentry [concept, linear-array and system diagrams on PDF pages 3–5](https://jedi-sec.us/downloads/Dept_Of_Defense/dod-lockheed_martin-passive-radar-salespitch.pdf).
- The open AM-radio array paper's [system, aircraft-truth and azimuth-Doppler figures](https://doi.org/10.1155/2021/3911956).
- ICRAR's [photograph of the 32-element, 35 m portable circular array](https://www.icrar.org/case-studies/space-domain-awareness/).
- VectorWarp site `Photo-1` through `Photo-3` are user-provided primary evidence in this task; they have no public source URL and are described in §2.3 without extracting hidden dimensions or architecture.

## 2. Antenna geometry comparison and VectorWarp numbers

### 2.1 Geometry sketches

```text
5-element regular polygon / UCA-like          sector ULA
                  1                            o--o--o--o--o  -> boresight sector
             /
          2      +      0                     planar grid
             \         /                      o--o--o
                3---4                          o--o--o

stacked/cylindrical                           sparse/custom
             o   o                             o       o
             o   o                               o
             o   o                           o          o
          azimuth + elevation                  large aperture, irregular lobes
```

| Geometry | Strength | Fundamental weakness | Calibration burden | VectorWarp relevance |
|---|---|---|---|---|
| Circular / regular polygon | Nearly uniform 360° azimuth behavior; no mechanical scan; cyclic beam bank is natural. | Small `M` gives broad beams and high ambiguity correlation; coplanar phase primarily measures horizontal direction cosine, so elevation changes bias azimuth/manifold matching. | Cyclic channel-order/heading ambiguity; coupling and mounting are direction-dependent. | Best match to the five-element pentagon, but only with a measured installed manifold and explicit ambiguity retention. |
| Linear | Maximum aperture per element in one dimension; simple steering, Toeplitz covariance and mature spatial smoothing under its assumptions. | Front/back ambiguity; scan loss and widening toward endfire; not 360° alone. | Easier geometry survey, but element patterns and end effects remain. | Useful only if the mission can be narrowed to a sector or several physical faces are added. Do not apply ULA-only forward/back smoothing to the current pentagon. |
| Planar grid | Resolves two direction cosines over one hemisphere and can estimate elevation if aperture is adequate. | More elements/channels; planar arrays still have upper/lower hemisphere ambiguity without element/directivity constraints. | Full 2-D manifold, position and polarization control. | A future 2-D non-coplanar or sufficiently large planar receive array could support elevation; five channels are too few for a dense grid. |
| Stacked circular / cylindrical | 360° azimuth plus vertical baselines for elevation; can use rings/faces independently. | Channel count, cost and mutual coupling rise; calibration becomes frequency/elevation dependent. | Requires 3-D surveying and a 3-D installed manifold. | Best future single-site geometry if real elevation/hemisphere discrimination is required. |
| Sparse / non-uniform | Larger aperture with fewer channels; can reduce repeated grating structure and narrow the main lobe. | Irregular sidelobes, calibration sensitivity and frequency-dependent ambiguities; optimization can overfit an ideal model. | Exact coordinates and measured manifold are mandatory. | Attractive after the present five-channel site is understood, not as the first intervention. |

### 2.2 Numerical analysis of the 148 mm pentagon

For a regular pentagon of radius `R = 0.148 m`:

- adjacent chord: `2 R sin(36°) = 0.17398 m`;
- largest vertex-to-vertex chord: `2 R sin(72°) = 0.28151 m`;
- physical diameter: `0.296 m` (slightly larger than the largest sampled baseline because no element lies exactly opposite another in an odd-sided polygon).

| RF | Wavelength | Radius | Adjacent spacing | Longest sampled baseline |
|---:|---:|---:|---:|---:|
| 527 MHz | 568.87 mm | 0.260 λ | 0.306 λ | 0.495 λ |
| 539 MHz | 556.20 mm | 0.266 λ | 0.313 λ | 0.506 λ |
| 550 MHz | 545.08 mm | 0.272 λ | 0.319 λ | 0.516 λ |
| 557 MHz | 538.23 mm | 0.275 λ | 0.323 λ | 0.523 λ |

Consequences:

1. **This is a small aperture.** At 550 MHz its longest sampled baseline is only 0.516 λ. An ideal, uniformly weighted five-element conventional beam steered in the array plane has an approximately **77° full 3 dB width** in a direct numerical array-factor calculation. It has no sharp grating lobe in that ideal cut, but the opposite-direction response is approximately −8.1 dB. Real element patterns, roof scattering and calibration error can be worse. The 7.5° spacing of a 48-beam fine scan is numerical interpolation, not 7.5° physical resolution.
2. **The usual “less than half-wavelength” rule is not a complete alias proof.** Adjacent elements are safely below 0.5 λ, while the longest chord crosses 0.5 λ around the current band. For a UCA/custom array, ambiguity should be evaluated from the full normalized manifold correlation over azimuth and elevation, not from one spacing scalar.
3. **Five complex channels provide few adaptive degrees of freedom.** After one unity-gain constraint, at most four exact independent spatial constraints exist in an ideal full-rank snapshot. Estimating a covariance, multiple coherent paths and a target from local data consumes those degrees rapidly. Diagonal loading and explicit fallback are essential.
4. **Elevation is structurally weak.** All nominal phase centres are coplanar and the elements are vertical. A horizontal ring observes horizontal direction cosine; changing elevation contracts the phase progression. Near overhead, azimuth information collapses. A 2-D azimuth-only manifold must therefore gate elevation, as VectorWarp already does.
5. **Small physical errors are large phase errors.** At 550 MHz, a 10 mm free-space path error is 6.6° of phase; 5°, 10° and 20° correspond to approximately 7.6, 15.1 and 30.3 mm of free-space path respectively. Cable velocity factor changes cable-length equivalents. Millimetre-level repeatability matters even though the conventional beam is broad, because adaptive nulls and super-resolution spectra are much more sensitive.
6. **2.4 MS/s and 200 ms CPI set orthogonal limits.** A full 2.4 MHz occupied waveform gives approximately `c/B = 125 m` excess-path sample spacing; a 200 ms CPI gives 5 Hz Doppler-bin spacing. A nominal 10 Hz output cadence with 200 ms CPI necessarily overlaps or otherwise reuses time support, so adjacent frames are not ten independent trials per second. At 550 MHz, 5 Hz corresponds to roughly 1.36 m/s under the monostatic `f_d = 2v/λ` special case; bistatic velocity projection depends on geometry.

### 2.3 Site-photo assessment

Evidence: three user-provided photographs received 2026-10-22 (`Photo-1`, `Photo-2`, `Photo-3`). They are treated as site records, not metrology.

**Visible observations only:**

- a central star-like support carries multiple thin vertical elements around its perimeter;
- the assembly is on a mast immediately beside a brick chimney and close to a shingled roof edge;
- a larger brick wall/building face is nearby and occupies a substantial angular sector in two views;
- metal chimney flashing/cap, mast hardware, brackets and several cables are close to the aperture;
- overhead/side wires or guy-like lines cross the nearby scene;
- trees, shrubs, fencing and seasonally variable vegetation are close to the installation;
- the mounting appears elevated above the lower roof but not in an unobstructed free-space test range.

**RF hypotheses to test, not facts inferred from the photographs:**

- the brick wall, roof, chimney metalwork and cable/mast currents may create direction- and frequency-dependent image currents and coherent multipath;
- unequal proximity of elements to the chimney/wall may distort element patterns and create channel-dependent amplitude span;
- vegetation and moisture changes may create slow nonstationarity;
- nearby conductors may make a geometry-only steering model inaccurate even if receiver channel phase is perfectly calibrated.

The photographs do **not** establish exact radius, element electrical length, channel mapping, true-north orientation, cable equality, polarization purity, impedance, ground-plane behavior or any reflector delay. Those require survey/VNA/known-source measurements. The visual environment is, however, consistent with the repository's measured Ford Street result: channel imbalance and phase response vary with transmitter direction and changed after a roof move. That agreement supports a measurement campaign, not a causal attribution to any one visible object.

## 3. Calibration and manifold report

### 3.1 Five different calibration problems

| Layer | What it corrects | Suitable evidence | What it cannot prove |
|---|---|---|---|
| Receiver sample/phase coherence | tuner/sample offset and receiver-chain phase drift | Kraken internal switched-noise correlation; fresh phase-ready flags | cable/antenna/site phase centre, mapping, heading, installed pattern |
| Cable/front-end transfer | cable delay, amplitude and receiver response versus frequency | VNA S-parameters or known injected signal for every complete path | mutual coupling and environmental scattering |
| Geometric survey | nominal phase-centre coordinates and heading | physical channel-by-channel identification and survey | electrical phase centre or installed response |
| Installed array manifold | element pattern, coupling, mast/roof/building response as a function of direction, elevation and frequency | far-field known sources or independently labelled aircraft passes, normalized to remove unknown target complex amplitude | behavior outside sampled frequency/elevation/environment; causal target gain |
| Operational drift | time-varying common/channel phase and gain | recurring reference-source/eigenvector logs and bounded recalibration | a fixed direction-dependent site error |

Kraken's internal calibration is necessary but deliberately narrower than array calibration. Official Kraken documentation says one common clock plus a switched broadband noise source is used to correlate each receiver against CH0 and compensate sample/phase differences. It also documents optional cable-delay/Touchstone correction. [Kraken design](https://github-wiki-see.page/m/krakenrf/krakensdr_docs/wiki), [web-control calibration options](https://github-wiki-see.page/m/krakenrf/krakensdr_docs/wiki/05.-KrakenSDR-Web-Interface-Controls).

### 3.2 Recommended VectorWarp calibration sequence

1. **Freeze an installation identity.** Record frequency, gain vector, channel-to-physical-element mapping, coordinates, element/mast state, cable identity, receiver build, reference mode, and photographs. Any change creates a new corpus.
2. **Verify receiver and cable layers.** Require fresh Kraken lag/phase calibration. Measure each cable/front-end path with a VNA across the actual occupied band where practical. Retain raw S-parameter files rather than only one phase number.
3. **Break the pentagon symmetry physically.** One-channel-at-a-time injection or disconnection must identify DAQ order. A regular pentagon has a cyclic equivalence: rotating channel labels by 72° is algebraically equivalent to rotating heading. Aircraft fitting alone cannot distinguish them.
4. **Run multiple known fixed RF sources.** Use at least two good-coherence, low-amplitude-span sources from separated bearings to check handedness and phase-axis consistency. Reject sources that fail a pre-registered coherence/span gate; do not average a clearly multipath-contaminated source into a “correction.”
5. **Collect direction/frequency/elevation manifold data.** Normalize each sample by a common complex scalar. Partition by site fingerprint, RF channel and gain. Gate high elevation because a coplanar azimuth model cannot represent it. Prefer a calibrated drone beacon or surveyed source for direct manifold measurement; if ADS-B aircraft are used, they are offline labels only.
6. **Fit robustly and cross-fit by whole pass.** A pass, not a frame, is the independent unit. Fit on complete training passes and score untouched passes from different times/aircraft/sectors. Never use a pass to estimate its own residual phase/gain.
7. **Require reproducible baseline improvement.** Compare against the ideal free-space manifold and a heading-only model on the exact same held-out passes. Require no important sector regression and retain all ambiguity lobes.
8. **Monitor drift separately.** Stable channel drift plus repeatable wrong bearing suggests installed response; moving phase correlated with error suggests receiver/cable drift. Do not allow an adaptive reference eigenvector to silently redefine absolute bearing.

### 3.3 What the inspected repository already does

The audit found the following implemented paths, not proposals:

- `process.reference_synthesis.mode: array_eigenbeam` across all five channels in `config/config-kraken.yml`;
- `coherent_beam_bank` and GPU-only `noise_whitened_beam_bank`, with `effective_beam_trials` and a bounded `direct_path_null_db`;
- conventional coherent bearing and candidate-local `covariance_mvdr` with diagonal loading, guard/training rings and fallback on singular covariance;
- an optional measured `manifold_file` whose frequency/channel dimensions are validated at startup;
- a recorder that associates sparse accepted cells with fresh ADS-B only for calibration evidence;
- automatic manifold fitting and **whole-pass-balanced held-out A/B promotion gates**;
- a channel-order/heading diagnostic and separate multi-source reference survey;
- a read-only frequency-resolved multipath shadow;
- raw pre-calibration ADC headroom telemetry plus calibrated-wire checks and safe gain rollback.

The repository also explicitly excludes experimental neural, MUSIC and SBL selectors from production bearing. The offline bounded recovery lane can compute Bartlett/Capon/MUSIC likelihoods under a frozen corridor and counts source/decorrelation/mapping/calibration choices. This is the correct distinction: “implemented offline diagnostic” is not “qualified production capability.”

The current site evidence in `docs/FORD_STREET_BEARING_SURVEY_20261019.md` is adverse: two quality-gated fixed sources supported forward/identity handedness, while other sources showed large direction-dependent amplitude imbalance; pre-move median target bearing error was 53.83°, and a post-move snapshot was 159.16°. A heading-only fit could cosmetically reduce error but was rejected as a physical heading estimate. This is strong repository evidence against applying one global heading/gain correction.

## 4. Direct-path and dynamic-range mitigation plan

### 4.1 The critical boundary: before versus after ADC

```text
antenna/site -> limiter/preselector/attenuation -> LNA/tuner -> ADC -> coherent calibration
     |                 PRE-ADC protects headroom             |       POST-ADC DSP
     |                                                       +-> reference beam
     +-> spacing, shielding, element pattern                         ECA/cancellation
                                                                    spatial null/MVDR
                                                                    CAF/CFAR/tracking
```

Digital cancellation can reduce the direct-path pedestal, zero-Doppler clutter and ambiguity sidelobes **only after the samples are valid**. It cannot reconstruct a weak echo lost to ADC clipping, front-end compression, intermodulation or quantization. The 2018 FM passive-radar analog-DPIS experiment explicitly targeted low-resolution SDR headroom and reported about 20 dB laboratory suppression and 15 dB field detection improvement; these measured numbers belong to that apparatus, not to VectorWarp. [Paper PDF](https://radarlab.hvt.bme.hu/publ/peto/2018/PetoDudasSeller_ADPIS_FM.pdf), DOI [10.1109/RADIOELEK.2018.8376381](https://doi.org/10.1109/RADIOELEK.2018.8376381).

### 4.2 Ordered mitigation stack

| Order | Intervention | Domain | VectorWarp-specific note |
|---:|---|---|---|
| 1 | Measure raw rails, near-rails, RMS/histograms and tuner readback on every channel under worst credible direct-path conditions. | Pre-ADC evidence | Already supported by the patched receiver/API. Absence of sampled clipping still does not prove analog linearity. |
| 2 | Select a fixed common gain, or a verified per-channel fixed gain vector, leaving margin for fading and other blockers. | Analog gain | Per-channel gain improves headroom balance but changes noise figure and manifold; it requires a new site fingerprint and held-out requalification. |
| 3 | Improve siting: increase clearance from wall/roof/chimney/metal; shorten and secure cables; preserve symmetry; document seasonal state. | Propagation/RF | Low-cost physical intervention with potentially larger benefit than more DSP, but benefit is a hypothesis until paired measurements. |
| 4 | Add a high-linearity band-select or channel preselector and, if needed, attenuation ahead of overload-prone stages. | Pre-ADC RF | It rejects out-of-band blockers, not the in-band illuminator/echo pair. Measure insertion loss, group delay and channel match. |
| 5 | If headroom remains limiting, prototype coherent analog vector cancellation with one or more delayed taps. | Pre-ADC RF/IF | High risk on a wideband TV waveform and roof multipath. A one-tap null may drift or cancel useful directions. Keep a bypass/reference recording and prove linearity/yield. |
| 6 | Stabilize/reference-synthesize from multiple elements and cancel a delayed/Doppler clutter subspace. | Post-ADC DSP | VectorWarp already has eigenbeam reference synthesis and clutter processing. ECA/ECA-S is a candidate extension; protect low-Doppler aircraft and use held-out trials. |
| 7 | Apply a bounded spatial direct-path constraint/null and covariance whitening. | Post-ADC spatial | Already available in bounded forms. With five channels, aggressive nulls can distort broad target response; sweep null depth only on training passes. |
| 8 | Combine multiple RF channels/illuminators at decision/track level. | Diversity | Professional systems repeatedly use this. Keep per-illuminator evidence visible; do not let one strong, contaminated channel dominate. |

### 4.3 Algorithm evidence and selection

- **Conventional digital beamforming / Bartlett:** demonstrated by Warsaw and TNO and robust when the manifold is known. Broad but diagnostically honest for five elements.
- **MVDR/Capon:** useful for suppressing spatial interference when covariance is representative and well-conditioned. VectorWarp's local guard/training ring, loading and fallback are necessary safeguards. A strong coherent roof path can make MVDR place a stable but wrong null/peak.
- **MUSIC:** mature for source DOA and present in Kraken's official direction-finding software; an academic range-Doppler-angle paper names MUSIC for direct-path DOA but presents conventional beamforming in its simulated chain. MUSIC needs a defensible source count, adequate independent snapshots and a correct manifold. Coherent multipath violates its simplest assumptions. Keep it shadow/offline until it beats conventional/MVDR on held-out real aircraft without sector regressions.
- **Forward-backward averaging/spatial smoothing:** demonstrated in array processing, but the common Toeplitz/ULA form must not be transferred to a five-element pentagon. Kraken documentation calls its decorrelation controls experimental and geometry-dependent.
- **ECA family:** the 2009 multistage ECA paper and later ECA-S work cancel direct-path/multipath subspaces constructed from delayed/Doppler-shifted reference replicas. ECA-S addresses time variation but can attenuate slow targets if adaptation is too aggressive. Its success criterion must include preserved low-Doppler aircraft, not only lower residual clutter power.
- **Frequency-resolved manifold/constraint:** particularly relevant when several coherent paths fall inside one 2.4 MS/s delay cell. Phase slope across occupied RF can distinguish delays below the coarse CAF sample spacing. VectorWarp already records a shadow diagnostic; promotion must remain separately held out.

## 5. Ranked VectorWarp roadmap

Expected benefits below are hypotheses unless a cited existing result is explicitly named. “Pass” always means evaluation on frozen, untouched whole aircraft passes plus negative periods; never per-frame resampling.

### A. Software-only / measurement-first

| Rank | Action | Rationale and expected benefit | Difficulty | Hardware | Principal failure modes | Validation and pre-registered criteria |
|---:|---|---|---|---|---|---|
| A1 | **Make RF headroom/linearity a mandatory evidence gate for every experiment.** | Prevents wasting DSP work on clipped/compressed input; expected to reduce irreproducible failures. | Low | Existing patched Kraken/site | Short probes miss rare overload; “no clips” mistaken for linearity. | Two independent captures per condition; zero rails, existing peak/P99.9 limits, stable readback. Fail on any unsafe channel or unexplained intermodulation in a two-tone/blocked-band test. |
| A2 | **Complete a clean installed-manifold campaign with physical mapping and multi-source acceptance.** | Attacks the measured dominant error: direction-dependent installed response. Expected largest bearing improvement. | Medium-high | Optional VNA/calibrated beacon or drone | Learns aircraft/illuminator contamination; cyclic mapping alias; poor sector/elevation coverage; seasonal drift. | Freeze mapping by physical test. At least current repository promotion minima (40 passes, 12 aircraft, six sectors; 12 held-out passes/four sectors), median ≤45°, ≥5° or ≥15% improvement, no within-45° loss, no sector >8° regression. Prefer stricter thresholds before publication. |
| A3 | **Use the existing frequency-resolved shadow to classify coherent roof/direct-path structure by RF channel and weather/site state.** | Tests whether whole-band rank-one calibration hides resolvable path slope. Expected to identify when fixed-site constraints are stable. | Medium | None | Diagnostic over-interpreted as target improvement; too little occupied bandwidth/SNR; fingerprint mixing. | Repeatability across days and known sources; fixed candidate chosen on training dates must reduce held-out bearing error or direct-path leakage without target-yield loss. Otherwise remain diagnostic. |
| A4 | **Run a frozen conventional-vs-MVDR-vs-offline-MUSIC study on identical complex cutouts.** | Determines whether adaptivity/super-resolution adds information after proper calibration. Expected outcome may be that conventional DBF wins in this aperture. | Medium | None | Method/peak/source-count cherry-picking; dependent frames; covariance includes target; multipath creates extra sources. | Pre-register method/source-count/loading family and charge all trials. Pass only if pass-median error and within-45° rate improve with no sector regression and stable multi-lobe margin on untouched passes. |
| A5 | **Evaluate ECA-S or a low-rank delayed/Doppler reference subspace in shadow.** | May reduce direct-path/multipath sidelobes beyond the existing clutter path. | High | None | Slow-target self-cancellation; ill-conditioned solve; compute deadline loss; reference contamination. | Paired replay on positives and negative periods. Pass: improved target/nearby-background contrast and recovery count at fixed family-wise false alarm, low-Doppler no-regression, and CPI p99 below deadline. Fail on any target erasure or threshold retuning after test inspection. |
| A6 | **Exploit multiple illuminators as independent evidence, not a pooled opaque score.** | Provides geometry/frequency diversity professional systems rely on. Expected to reduce site-specific blind sectors. | Medium-high | Retuning or additional simultaneous RF capacity | Non-simultaneous target state; correlated errors; multiple-comparison inflation. | Pre-register per-illuminator eligibility and fusion. Require pass-level improvement over the best single illuminator with honest missing-data handling and no ADS-B inference. |

### B. Low-cost physical/RF changes

| Rank | Action | Rationale and expected benefit | Difficulty | Hardware | Principal failure modes | Validation and criteria |
|---:|---|---|---|---|---|---|
| B1 | **Temporary clear-space mast A/B before permanent rebuilding.** | Directly tests whether the roof/wall/chimney environment dominates the manifold. Expected benefit: lower amplitude span and more stable phase-versus-bearing. | Medium | Temporary nonconductive mast, weatherproof cabling | Changed height/coverage confounds site effect; unsafe mounting; cable changes. | Alternate original/clear location with same RF, gain and hardware; known-source coherence/span plus whole-pass errors. Pass if improvements repeat after returning to the original site and again after redeployment. |
| B2 | **Matched channel preselectors/attenuators and a documented fixed gain vector.** | Protects analog linearity from out-of-band/in-band blockers while preserving repeatability. | Medium | Five matched filters/attenuators or high-linearity front ends; VNA access | Group-delay mismatch, noise-figure loss, filter temperature drift; no help against same-channel direct path. | Bench S-parameters and two-tone/blocker test, then paired site captures. Pass only with increased blocker margin and no held-out aircraft-yield/bearing loss. |
| B3 | **Dedicated reference experiment.** | Separates reference quality from the surveillance manifold, as Silent Sentry publicly did. | Medium | With current Kraken: sacrifice one element (4+1) or add a truly synchronized sixth channel | Four-element aperture worsens bearing; separate receiver loses coherence; reference multipath. | Frozen 5-element eigenbeam vs 4+1 comparison on same illuminator/time blocks. Pass only if clutter/detection improvement exceeds bearing/aperture loss. |
| B4 | **Calibrated far-field beacon/drone survey at several azimuths and elevations.** | Provides known waveform/direction independent of target RCS and ADS-B association. | Medium-high | Legal/safe transmitter or receive-only use of surveyed broadcast sources; optional drone operator | Near-field operation, polarization mismatch, regulatory/safety issues, position error. | Far-field distance and position uncertainty documented; repeated clockwise/counter-clockwise sweeps. Pass if independent repetitions agree within the publication error budget. |

### C. Future hardware

| Rank | Action | Rationale and expected benefit | Difficulty | Hardware | Principal failure modes | Validation and criteria |
|---:|---|---|---|---|---|---|
| C1 | **Move to 8+ coherent channels with a larger nonuniform ring or two staggered rings.** | More aperture and covariance degrees of freedom; maintains 360°. Expected narrower lobes and better interference rejection. | High | New coherent receiver, array, enclosure, cabling | Grating/irregular lobes, calibration explosion, compute growth. | Full-wave/manifold simulation followed by measured chamber/open-range response; held-out real-aircraft comparison at fixed false alarm and latency. |
| C2 | **Add vertical baselines (stacked ring/cylindrical array).** | Resolves elevation dependence that the current coplanar ring cannot. | High | Additional coherent channels and surveyed 3-D structure | Vertical multipath, channel count, upper/lower ambiguity, sparse-elevation lobes. | 3-D calibrated source campaign; require azimuth no-regression and bounded elevation error across independently held elevations. |
| C3 | **Use higher-linearity, higher-effective-dynamic-range per-element receivers with common clock/calibration injection.** | Attacks the direct-path/echo dynamic-range problem before DSP. | High/costly | 12–14-bit-class coherent receiver/front ends, preselection, common timing | Nominal bit depth without ENOB/linearity; channel skew; data/compute rate. | Cascaded NF/IIP3/SFDR bench test plus same-site target replay; pass on blocker margin and target yield, not ADC bits alone. |
| C4 | **Multi-tap adaptive analog direct-path cancellation ahead of each surveillance ADC.** | Can recover headroom when the desired echo and direct path occupy the same RF channel. | Very high | Coherent reference coupler, vector modulators/delay taps, control loop, bypass/monitor paths | Drift, imperfect wideband null, target/self-cancellation, instability, calibration burden. | Start one channel with bypassed simultaneous control. Require repeatable pre-ADC suppression, verified linearity/noise, and held-out detection improvement; auto-bypass on coherence or headroom fault. |

## 6. Evaluation discipline for ADS-B-free inference

ADS-B can label calibration and evaluation data; it must not select a live lobe, update a live manifold, resolve a candidate, tune a threshold after test inspection, or enter the deployed feature vector.

### Required experimental contract

1. **Declare the causal question.** Example: “Does a fixed manifold trained before date X improve radar-only bearing on later whole passes at the same fixed false-alarm policy?”
2. **Freeze the macro and family.** Record source code/config/input hashes, RF/gain/site fingerprint, method arms, hyperparameter counts and stopping rules before the held-out run.
3. **Split by whole aircraft pass and time block.** Never scatter correlated 10 Hz/overlapped CPI frames from one pass across train/test. Group same aircraft/day/config when leakage is plausible.
4. **Keep positive, negative and miss cohorts.** Positive ADS-B-correlated passes measure sensitivity and bearing; quiet/negative intervals measure false alarms; a pre-existing release-miss queue is inspected only after calibration receipts are frozen.
5. **Charge every tried arm.** Beam count, null depth, mapping, source count, loading, decorrelation and calibration variants belong in the family-wise trial count. Do not report the best uncharged spectrum.
6. **Use paired pass-level statistics.** Report median angular error, within-45° rate, sector medians, detection/recovery at fixed false alarm, uncertainty coverage and latency. Bootstrap by pass, not frame.
7. **Preserve raw ambiguity.** A top-1 bearing is not “resolved” unless the independent lobe margin, temporal consistency and aperture-derived uncertainty gates pass. Keep alternative lobes and unresolved states.
8. **Fail closed.** Geometry, mapping, orientation, calibration freshness, headroom or site fingerprint mismatch disables bearing publication but must not disable ordinary passive-radar detection.

### Recommended release gates

- **Detection:** more independent aircraft passes recovered at the same pre-registered family-wise false-alarm policy, with no meaningful loss in already detected passes.
- **Bearing:** held-out pass-median error improves over the ideal manifold by the pre-registered margin; within-45° rate does not fall; no adequately sampled sector regresses materially; reported uncertainty contains truth at its advertised rate.
- **Dynamic range:** zero observed clipping under bounded tests, measured linear blocker margin, and no unexplained spur growth. “No sampled rails” alone is insufficient.
- **Operations:** CPI processing p99 stays below the real cadence deadline, state remains bounded, and failure/restart paths preserve publication interlocks.
- **Causality:** a physical A/B intervention reproduces after reversal/redeployment, or a software model is trained only on earlier/different complete passes and evaluated once on untouched later passes.

## 7. Bottom line for the present installation

1. Keep the pentagon for now; it is a sensible five-channel 360° geometry, but it is a **broad, ambiguity-prone aperture**, not a precision interferometer by scan-bin count.
2. Do not apply a global heading or gain fix from the current aircraft corpus. Repository evidence already shows direction-dependent response and a misleading heading-only fit.
3. Use VectorWarp's existing headroom, manifold, pass-holdout and frequency-resolved tools before adding production algorithms.
4. Run a reversible clear-space mast experiment. If fixed-source amplitude span/phase consistency and held-out bearing improve, invest in the site before investing in MUSIC.
5. If the site cannot be cleared, a higher-dynamic-range receiver and/or pre-ADC cancellation may be required, but only raw headroom/linearity data can justify it.
6. For genuinely accurate single-site 3-D ADS-B-free inference, plan for more coherent channels and vertical aperture. The current five coplanar monopoles cannot supply robust elevation from geometry alone.
