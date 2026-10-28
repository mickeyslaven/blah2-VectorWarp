const geometryEditor = typeof module !== 'undefined' && module.exports ?
  require('./kraken_geometry') : window.KrakenGeometry;
const CONFIG_SAVE_TIMEOUT_MS = 90000;
const MEASURED_GAIN_SAVE_TIMEOUT_MS = 1250000;
const CONFIG_META = {
  'capture': ['Receiver', 'Radio hardware, tuning and replay'],
  'process': ['Radar', 'Timing, search area, filtering and tracking'],
  'network': ['Connectivity', 'Service addresses and ports'],
  'truth': ['ADS-B', 'Aircraft overlay'],
  'location': ['Sites', 'Receiver and illuminator positions'],
  'save': ['Recording', 'Files and recording folder'],
  'save.retention': ['Storage retention', 'Keep recordings bounded by age, total storage, and free disk space.'],
  'save.retention.automatic_cleanup': ['Delete old recordings automatically', 'Off until you enable it. Only completed, inactive recordings are removed.'],
  'save.retention.low_space_policy': ['When the disk reserve is reached', 'Stop recording or delete oldest aircraft clips. Manual recordings stay.'],
  'save.retention.maximum_age_days': ['Keep recordings for (days)', 'Oldest completed recordings become eligible for cleanup. Set 0 to disable the age limit.'],
  'save.retention.maximum_total_gb': ['Maximum recording storage (GB)', 'Cleanup removes the oldest completed recordings above this total. Set 0 for no size limit.'],
  'save.retention.minimum_free_gb': ['Always leave disk free (GB)', 'New recordings stop or are refused below this safety floor.'],
  'capture.fs': ['Sample rate', 'Radio samples per second. Kraken is fixed at 2.4 MS/s.'],
  'capture.fc': ['Center frequency', 'Tune in MHz, kHz and Hz.'],
  'capture.aircraft_clips': ['Automatic aircraft IQ clips', 'Receiver-independent bounded recordings triggered by eligible ADS-B aircraft passes.'],
  'capture.aircraft_clips.enabled': ['Record eligible aircraft automatically', 'Off by default. Requires ADS-B and a live receiver; manual recording is unaffected.'],
  'capture.aircraft_clips.pre_seconds': ['Seconds before aircraft event', 'Raw-IQ pre-roll retained before the eligible pass event.'],
  'capture.aircraft_clips.post_seconds': ['Seconds after aircraft event', 'Raw-IQ post-roll. Pre-roll plus post-roll cannot exceed 10 seconds.'],
  'capture.aircraft_clips.minimum_slant_range_km': ['Nearest aircraft range (km)', 'Ignore aircraft closer than this receiver slant range, in kilometres.'],
  'capture.aircraft_clips.maximum_slant_range_km': ['Farthest aircraft range (km)', 'Ignore aircraft beyond this receiver slant range, in kilometres.'],
  'capture.aircraft_clips.minimum_elevation_deg': ['Lowest aircraft elevation (°)', 'Ignore aircraft below this geometric elevation angle.'],
  'capture.aircraft_clips.maximum_elevation_deg': ['Highest aircraft elevation (°)', 'Ignore aircraft above this geometric elevation angle.'],
  'capture.aircraft_clips.minimum_interval_seconds': ['Minimum time between clips (s)', 'Limits recorder and disk load when several aircraft become eligible together.'],
  'capture.device': ['Receiver hardware', 'Settings for the selected radio.'],
  'capture.device.type': ['Receiver type', 'Choose your SDR.'],
  'capture.device.array_geometry': ['Antenna layout', 'Record your layout; this does not configure the receiver.'],
  'capture.device.channel_count': ['Input channels', 'Coherent Kraken channels sent by HeIMDALL. Allowed: 2–8.'],
  'capture.device.reference_channel': ['Reference channel', 'Input carrying the direct transmitter signal in dedicated mode.'],
  'capture.device.surveillance_channels': ['Radar inputs', 'Inputs searched for reflected signals. Select at least one.'],
  'capture.device.heimdall': ['HeIMDALL connection', 'KrakenSDR Suite V2 data source.'],
  'capture.device.heimdall.host': ['HeIMDALL host', 'Computer running HeIMDALL.'],
  'capture.device.heimdall.port': ['HeIMDALL port', 'HeIMDALL TCP data port.'],
  'capture.device.heimdall.control_port': ['HeIMDALL control port', 'Suite V2 status and command port. Usually 8092; separate from the IQ data port.'],
  'capture.device.heimdall.gain': ['Receiver gain', 'Shared gain in dB. Automatic adjustment updates this after checking all channels.'],
  'capture.device.heimdall.channel_gains': ['Coherent channel gains', 'Optional fixed dB vector, one physical tuner-table gain per channel.'],
  'capture.device.heimdall.gain_control': ['Gain adjustment', 'Automatic checks each Apply and monitors overload. Fixed holds gain for comparisons.'],
  'capture.device.address': ['USRP address', 'UHD key=value selector, such as type=b200 or type=b200,serial=…'],
  'capture.device.subdev': ['USRP subdevices', 'UHD mapping for the two receive channels.'],
  'capture.device.antenna': ['USRP antennas', 'Antenna port for each receive channel.'],
  'capture.device.gain': ['Channel gains', 'Gain for each receive channel, in dB.'],
  'capture.device.serial': ['HackRF serial numbers', 'Reference and surveillance device serial numbers'],
  'capture.device.gain_lna': ['HackRF LNA gains', 'Two gains in dB: 0–40, in steps of 8.'],
  'capture.device.gain_vga': ['HackRF VGA gains', 'Two gains in dB: 0–62, in steps of 2.'],
  'capture.device.amp_enable': ['HackRF RF amplifiers', 'Enable the RF amplifier for each receiver'],
  'capture.device.agcSetPoint': ['AGC target', 'RSPduo automatic-gain target, from -72 to 0 dBFS.'],
  'capture.device.bandwidthNumber': ['AGC speed', 'RSPduo automatic-gain response: off, 5, 50 or 100 Hz.'],
  'capture.device.gainReduction': ['Gain reduction', 'Two RSPduo gain reductions in dB. Each must be 20–59.'],
  'capture.device.lnaState': ['LNA state', 'Gain-reduction step; available states depend on frequency. 0 is least reduction.'],
  'capture.device.dabNotch': ['DAB notch filter', 'Suppress digital-audio broadcast interference'],
  'capture.device.rfNotch': ['RF notch filter', 'Enable the RSPduo RF notch filter'],
  'capture.replay': ['Replay', 'Use a recording instead of live radio.'],
  'capture.replay.state': ['Replay a recording', 'Use the replay file instead of live input'],
  'capture.replay.loop': ['Loop replay', 'Start the recording again after its end.'],
  'capture.replay.file': ['Replay file', 'Full path to the recording used when replay is enabled.'],
  'capture.replay.provenance': ['Replay provenance', 'Older recordings remain unqualified; decision-grade requires bounded RF-time provenance.'],
  'capture.replay.format': ['Recording format', 'Auto detects portable recordings; select a legacy format only when needed.'],
  'capture.replay.legacy_block_samples': ['Legacy USRP block samples', 'Required only for old USRP blocks; use the original receiver block size.'],
  'process.performance': ['Performance', 'CPU and GPU settings'],
  'process.performance.surveillance_workers': ['Surveillance workers', 'Auto chooses how many radar inputs to process in parallel.'],
  'process.performance.fft_threads': ['FFT threads per worker', 'Auto shares available CPU capacity with the parallel workers. Applies after restart.'],
  'process.performance.acceleration': ['Processing hardware', 'Automatic checks GPU accuracy and speed. Falls back to CPU if needed.'],
  'process.data': ['Frame timing', ''],
  'process.data.cpi': ['Frame interval (CPI)', 'Seconds per radar frame and display update. Applies after restart.'],
  'process.data.buffer': ['Buffered intervals', 'Number of processing intervals held in memory. Minimum 1; start with 2.'],
  'process.data.overlap': ['Frame overlap', 'Not used by this processor. Frames do not overlap.'],
  'process.ambiguity': ['Delay & Doppler', 'Limits of the radar map'],
  'process.ambiguity.delayMin': ['Minimum delay bin', 'First extra-path sample shown in the radar map.'],
  'process.ambiguity.delayMax': ['Maximum delay bin', 'Last extra-path sample shown in the radar map.'],
  'process.ambiguity.dopplerMin': ['Minimum Doppler', 'Lower Doppler limit in Hz'],
  'process.ambiguity.dopplerMax': ['Maximum Doppler', 'Upper Doppler limit in Hz'],
  'process.clutter': ['Clutter removal', 'Reduces stationary reflections and multipath.'],
  'process.clutter.enable': ['Remove clutter', 'Filter stationary background reflections.'],
  'process.clutter.followAmbiguity': ['Follow radar delay range', 'Cancel across the complete delay map; disable only for a manual filter range.'],
  'process.clutter.delayMin': ['Minimum filter delay bin', 'Manual first filter sample; ignored while following the radar delay range.'],
  'process.clutter.delayMax': ['Maximum filter delay bin', 'Manual exclusive filter endpoint; ignored while following the radar delay range.'],
  'process.reference_synthesis': ['Reference signal', 'Use one receiver input or combine inputs into the broadcast reference.'],
  'process.reference_synthesis.mode': ['Reference mode', 'Use one input or combine several inputs automatically.'],
  'process.reference_synthesis.channels': ['Combined inputs', 'Inputs used to create the synthesized reference signal.'],
  'process.reference_synthesis.analysis_samples': ['Analysis samples', 'Samples per reference estimate. Minimum: 64.'],
  'process.reference_synthesis.analysis_interval': ['Analysis interval', 'Frames between reference-weight updates'],
  'process.reference_synthesis.power_iterations': ['Solver iterations', 'More iterations improve the reference estimate but use more CPU.'],
  'process.reference_synthesis.covariance_smoothing': ['Reference smoothing', 'Weight given to the previous estimate, from 0 to 1.'],
  'process.reference_synthesis.diagonal_loading': ['Reference stability', 'Small value that keeps the reference solver stable.'],
  'process.surveillance_fusion': ['Surveillance fusion', 'Combine the ambiguity maps from coherent receiver channels.'],
  'process.surveillance_fusion.mode': ['Fusion mode', 'Keep RMS power fusion or scan a calibrated coherent 360-degree beam bank.'],
  'process.surveillance_fusion.beams': ['Coherent beams', 'Number of evenly spaced azimuth beams used around the full horizon.'],
  'process.surveillance_fusion.effective_beam_trials': ['Effective beam trials', 'Independent beam trials used to correct the detector false-alarm threshold.'],
  'process.surveillance_fusion.direct_path_null_db': ['Direct-path null', 'Attenuation applied toward the measured illuminator direction, in dB.'],
  'process.bearing': ['Bearing output', 'Publish radar-only azimuth candidates and bistatic locations on a separate opt-in interface.'],
  'process.bearing.enable': ['Enable bearing output', 'Needs GPU coherent tracking plus confirmed mapping, geometry, and true-north orientation.'],
  'process.bearing.method': ['Bearing estimator', 'Use the calibrated beam bank or local covariance MVDR candidate scoring.'],
  'process.bearing.reference_anchor': ['Bearing reference anchor', 'Follow the live eigenbeam or hold bearing refinement to the surveyed site response.'],
  'process.bearing.scan_beams': ['Bearing scan beams', 'Fine azimuth bins around 360 degrees; must be a surveillance-beam multiple.'],
  'process.bearing.candidate_peaks': ['Bearing candidates', 'Independent bearing lobes retained per detection; currently fixed at three.'],
  'process.bearing.guard_radius': ['Covariance guard radius', 'Target-neighborhood cells excluded from the local covariance estimate.'],
  'process.bearing.training_radius': ['Covariance training radius', 'Delay and Doppler neighborhood used to estimate local interference.'],
  'process.bearing.diagonal_loading': ['Covariance stability', 'Diagonal loading applied before adaptive bearing inversion.'],
  'process.bearing.max_rate_deg_s': ['Bearing motion limit', 'Maximum plausible azimuth rate used to retain and score multiple bearing lobes across a track. Zero preserves the legacy temporal histogram.'],
  'process.bearing.manifold_file': ['Measured array manifold', 'Optional v1 JSON calibration measured across the configured bearing scan. Empty uses ideal regular-polygon steering.'],
  'process.bearing.manifold_automation': ['Automatic manifold qualification', 'Fit and shadow-test installed-site calibration in a background worker.'],
  'process.bearing.manifold_automation.auto_promote': ['Promote qualified manifold', 'Activate only after withheld flight passes beat default steering.'],
  'process.bearing.channel_mapping': ['Bearing channel mapping', 'Bearing-only logical-channel to physical-element permutation.'],
  'process.bearing.frequency_resolved_shadow': ['Frequency-resolved multipath shadow', 'Measure coherent path delay structure without changing radar output.'],
  'process.bearing.frequency_resolved_shadow.enabled': ['Run multipath shadow', 'Analyze short read-only coherent captures in an isolated worker.'],
  'process.bearing.frequency_resolved_shadow.fft_size': ['Multipath FFT size', 'Transform length used to estimate cross-spectral channel response.'],
  'process.bearing.frequency_resolved_shadow.subbands': ['Multipath subbands', 'Number of frequency slices retained in each candidate model.'],
  'process.bearing.frequency_resolved_shadow.bandwidth_fraction': ['Analyzed bandwidth fraction', 'Central fraction of captured RF bandwidth used by the model.'],
  'process.bearing.frequency_resolved_shadow.diagonal_loading': ['Direct constraint loading', 'Stabilization applied when constructing shadow MVDR weights.'],
  'process.bearing.frequency_resolved_shadow.capture_seconds': ['Shadow capture duration', 'Coherent RF duration analyzed during each periodic run.'],
  'process.bearing.frequency_resolved_shadow.interval_s': ['Shadow analysis interval', 'Seconds between bounded frequency-resolved analyses.'],
  'process.bearing.frequency_resolved_shadow.timeout_ms': ['Shadow capture timeout', 'Maximum time allowed for one read-only receiver capture.'],
  'process.bearing.frequency_resolved_shadow.path': ['Multipath candidate path', 'Absolute path for the latest fingerprinted candidate model.'],
  'process.bearing.manifold_recorder': ['Shared array calibration corpus', 'Collect ADS-B-labelled complex responses once; bearing and detection fit separate models.'],
  'process.bearing.manifold_recorder.enabled': ['Record shared array samples', 'Eligible coherent Kraken sites collect automatically; turn this off to opt out.'],
  'process.bearing.manifold_recorder.path': ['Shared corpus path', 'Rotating JSONL evidence used by the separate bearing and detection fitters.'],
  'process.bearing.manifold_recorder.min_snr_db': ['Minimum calibration SNR', 'Discard weaker detected cells from calibration recording.'],
  'process.bearing.manifold_recorder.min_elevation_deg': ['Minimum aircraft elevation', 'Discard calibration aircraft below this geometric elevation angle.'],
  'process.bearing.manifold_recorder.max_elevation_deg': ['Maximum aircraft elevation', 'Discard steep aircraft angles that a planar azimuth manifold cannot resolve.'],
  'process.bearing.manifold_recorder.max_delay_error_km': ['Maximum delay error', 'Largest ADS-B-to-radar delay difference allowed for a calibration match.'],
  'process.bearing.manifold_recorder.max_doppler_error_hz': ['Maximum Doppler error', 'Largest ADS-B-to-radar Doppler difference allowed for a calibration match.'],
  'process.bearing.manifold_recorder.max_truth_age_s': ['Maximum ADS-B age', 'Discard calibration matches whose aircraft position is too old.'],
  'process.bearing.manifold_recorder.max_file_mb': ['Calibration file size', 'Rotate the active JSONL sample file at this size in MiB.'],
  'process.bearing.manifold_recorder.max_files': ['Calibration file count', 'Maximum number of active and rotated sample files retained.'],
  'process.bearing.min_consistent': ['Bearing consistency samples', 'Minimum fine-beam observations before an estimate can become a resolved map point.'],
  'process.bearing.min_peak_separation_db': ['Bearing lobe margin', 'Power margin required before one persistent lobe is considered resolved.'],
  'process.detection': ['Detection', 'Find targets in the radar map'],
  'process.detection.enable': ['Detect targets', 'Create detections from the radar map.'],
  'process.detection.pfa': ['False-alarm probability', 'Lower values reduce false detections but may miss weak targets.'],
  'process.detection.nGuard': ['Guard cells per side', 'Nearby cells excluded from the noise estimate.'],
  'process.detection.nTrain': ['Training cells per side', 'CFAR background cells on each side. More cells reduce threshold loss but cost more.'],
  'process.detection.minDelay': ['Minimum detection delay bin', 'Ignore detections below this delay sample'],
  'process.detection.minDoppler': ['Minimum absolute Doppler', 'Ignore detections closer to zero Doppler than this many Hz'],
  'process.detection.nCentroid': ['Position window', 'Cells used to refine each detection position.'],
  'process.detection.integration_cpis': ['Integrated intervals', 'Consecutive radar intervals combined before detection.'],
  'process.detection.integration_motion_bins': ['Motion search radius', 'Delay and Doppler bins searched between two integrated intervals.'],
  'process.detection.track_before_detect': ['Track before detect', 'Accumulate subthreshold motion-consistent energy before normal tracking.'],
  'process.detection.track_before_detect.enable': ['Enable track before detect', 'Run GPU 2/4/8-CPI motion banks and candidate-only coherent refinement.'],
  'process.detection.track_before_detect.max_cpis': ['Longest weak-target bank', 'Use 2/4 banks or 2/4/8 banks; longer banks add sensitivity and latency.'],
  'process.detection.track_before_detect.motion_bins': ['Trajectory search radius', 'Delay and Doppler cells allowed to move per CPI along a constant path.'],
  'process.tracker': ['Tracking', 'Joins repeated detections into persistent tracks.'],
  'process.tracker.enable': ['Track targets', 'Run the multi-frame tracker'],
  'process.tracker.initiate': ['Track confirmation', 'Detections needed to start a track'],
  'process.tracker.initiate.M': ['Required detections', 'Detections required inside the confirmation window'],
  'process.tracker.initiate.N': ['Confirmation window', 'Frames examined when starting a track'],
  'process.tracker.initiate.maxAcc': ['Maximum Doppler acceleration', 'Largest Doppler change allowed when starting a track, in Hz/s.'],
  'process.tracker.initiate.acceleration_model': ['Acceleration initiation', 'Choose duplicate acceleration hypotheses or one adaptive tentative track.'],
  'process.tracker.initiate.stationary_clutter': ['Persistent clutter initiation filter', 'Learn recurring low-Doppler cells and prevent only new tracks from starting there. Existing moving tracks may cross zero Doppler.'],
  'process.tracker.initiate.stationary_clutter.enable': ['Enable persistent clutter filter', 'Apply the learned clutter map only when starting a new GPU-resident track.'],
  'process.tracker.initiate.stationary_clutter.persistence_cpis': ['Clutter persistence', 'Repeated low-Doppler frames required before a cell blocks new track initiation.'],
  'process.tracker.initiate.stationary_clutter.max_doppler_hz': ['Stationary Doppler band', 'Absolute Doppler range learned as persistent clutter, in Hz.'],
  'process.tracker.track_guided_shadow': ['Track-guided recovery shadow', 'Test weak moving-track recovery from rolling CPI maps without changing live tracks.'],
  'process.tracker.track_guided_shadow.enable': ['Enable recovery shadow', 'Run the bounded GPU-resident counterfactual and report timing and would-recover track IDs.'],
  'process.tracker.track_guided_shadow.promote': ['Promote guided recoveries', 'Send guided observations to the normal tracker; disable for shadow-only evaluation.'],
  'process.tracker.track_guided_shadow.max_tracks': ['Maximum shadow tracks', 'Hard per-frame GPU cap for the selected tracks and their small CFAR windows.'],
  'process.tracker.track_guided_shadow.evidence_path': ['Recovery evidence path', 'Absolute JSONL path for ADS-B-labelled shadow recovery evidence.'],
  'process.tracker.track_guided_shadow.max_truth_age_s': ['Maximum ADS-B age', 'Discard recovery comparisons whose aircraft position is too old.'],
  'process.tracker.track_guided_shadow.max_file_mb': ['Recovery file size', 'Rotate the active recovery evidence file at this size in MiB.'],
  'process.tracker.track_guided_shadow.max_files': ['Recovery file count', 'Maximum number of active and rotated recovery files retained.'],
  'process.tracker.delete': ['Missed frames before deletion', 'Consecutive misses allowed before a track is removed'],
  'process.tracker.smooth': ['Track smoothing', 'Reserved setting; not used by this processor.'],
  'process.tracker.gpu_capacity': ['GPU track capacity', 'Maximum simultaneous tracks retained in GPU memory.'],
  'process.tracker.beam_tolerance': ['Beam association tolerance', 'Maximum circular beam-index difference allowed when joining detections.'],
  'network.ip': ['API / data IPv4 address', 'IPv4 address shared by data connections and listeners. Must reach the API host.'],
  'network.ports': ['Service ports', 'TCP ports used by the API and radar data streams'],
  'network.ports.api': ['Web API port', 'Browser data port. Reopen with ?apiPort=PORT after changing it.'],
  'network.ports.map': ['Radar map port', 'Processed map input stream'],
  'network.ports.detection': ['Detection port', 'Detected-return input stream'],
  'network.ports.track': ['Track port', 'Target-track input stream'],
  'network.ports.bearing': ['Bearing port', 'Opt-in radar bearing input stream'],
  'network.ports.manifold': ['Calibration port', 'Opt-in sparse complex manifold-evidence stream'],
  'network.ports.timestamp': ['Timestamp port', 'Radar frame timestamp stream'],
  'network.ports.timing': ['Timing port', 'Processing-performance stream'],
  'network.ports.iqdata': ['IQ metadata port', 'Radio-sample metadata stream'],
  'network.ports.config': ['Configuration port', 'Reserved for configuration control; it must remain unique.'],
  'truth.adsb': ['ADS-B', 'Aircraft overlay and radar comparison'],
  'truth.adsb.enabled': ['Show ADS-B', 'Display aircraft from the ADS-B feed'],
  'truth.adsb.tar1090': ['ADS-B sources', 'Add local receivers or public exchanges; the newest fresh source wins without blending.'],
  'truth.adsb.poll_interval': ['Poll interval', 'Seconds between raw ADS-B reads.'],
  'truth.adsb.smoothing_window': ['Motion smoothing', 'Recent position updates used for Doppler.'],
  'truth.adsb.max_position_age': ['Maximum position age', 'Ignore aircraft positions older than this many seconds.'],
  'truth.adsb.aircraft_photo_provider': ['Aircraft photos', 'Optional photos shown only after selecting an aircraft.'],
  'truth.adsb.provisional_event_time_uncertainty_s': ['Provisional UTC uncertainty', 'Seconds of SDR event-time uncertainty included in validation gates.'],
  'truth.ais': ['AIS (not implemented)', 'Reserved maritime-feed settings; preserved but not editable.'],
  'truth.ais.enabled': ['AIS evaluation', 'Not implemented in this processor.'],
  'truth.ais.ip': ['Reserved AIS address', 'Not used by this processor.'],
  'truth.ais.port': ['Reserved AIS port', 'Not used by this processor.'],
  'location.rx': ['Receiver site', 'Antenna position used for range geometry.'],
  'location.tx': ['Illuminator site', 'Broadcast transmitter position used for range geometry.'],
  'location.rx.latitude': ['Latitude', 'Decimal degrees north'],
  'location.tx.latitude': ['Latitude', 'Decimal degrees north'],
  'location.rx.longitude': ['Longitude', 'Decimal degrees east'],
  'location.tx.longitude': ['Longitude', 'Decimal degrees east'],
  'location.rx.altitude': ['Altitude', 'Meters above mean sea level'],
  'location.tx.altitude': ['Altitude', 'Meters above mean sea level'],
  'location.rx.name': ['Site name', 'Receiver location name'],
  'location.tx.name': ['Site name', 'Transmitter location name'],
  'save.iq': ['Legacy IQ flag', 'Spacebar recording controls raw IQ; this legacy value is preserved.'],
  'save.map': ['Record radar maps', 'Write processed radar maps to disk'],
  'save.detection': ['Record detections', 'Write detected targets to disk'],
  'save.timing': ['Timing file flag', 'Reserved value; live timing is streamed but not file-saved here.'],
  'save.path': ['Recording folder', 'Directory used for saved data']
};

let activeConfig = null;
let originalConfig = '';
let capabilities = {};
let restartInProgress = false;
let pendingLiveConfig = null;
let configSyncInProgress = false;
let validationTimer = null;
let validationPending = false;
let validationErrors = [];
let validationNotices = [];
let validationSequence = 0;
let configRevision = null;
let pendingRevision = null;
let validatedSnapshot = null;
let saveInProgress = false;
let upstreamPending = false;
let restartRetry = false;
let receiverApplyPending = false;
let recentSites = [];
let receiverInvalidatedGeometry = false;
let lastDetectedReceiverTypes = new Set();
let settingsMode = 'advanced';
let settingsModeStored = false;
let activateSettingsTab = () => {};
try {
  const storedMode = window.localStorage.getItem('vectorwarp-settings-mode');
  if (storedMode === 'advanced' || storedMode === 'simple') {
    settingsMode = storedMode;
    settingsModeStored = true;
  }
} catch (_) { /* Remembering the preferred detail level is optional. */ }

const SIMPLE_SETTING_PATHS = new Set([
  'capture.fc', 'capture.fs', 'capture.device.type', 'capture.device.channel_count',
  'capture.device.reference_channel', 'capture.device.surveillance_channels',
  'capture.device.serial', 'capture.device.address', 'capture.device.antenna',
  'capture.device.subdev', 'capture.device.gain', 'capture.device.gainReduction',
  'capture.device.gain_lna', 'capture.device.gain_vga', 'capture.device.amp_enable',
  'capture.device.lnaState',
  'capture.device.agcSetPoint', 'capture.device.bandwidthNumber',
  'capture.device.heimdall.host', 'capture.device.heimdall.port',
  'capture.device.heimdall.control_port', 'capture.device.heimdall.gain',
  'capture.device.heimdall.gain_control',
  'capture.replay.state', 'capture.replay.file', 'capture.replay.loop',
  'process.data.cpi', 'process.clutter.enable', 'process.detection.enable',
  'process.tracker.enable', 'process.performance.acceleration',
  'truth.adsb.enabled', 'truth.adsb.tar1090'
]);
function simpleSettingHidden(path) {
  if (settingsMode === 'advanced') return false;
  if (path === 'network' || path.startsWith('network.')) return true;
  if (path.startsWith('save.') || path.startsWith('location.')) return false;
  if ([...SIMPLE_SETTING_PATHS].some(allowed => allowed.startsWith(`${path}.`))) return false;
  return !SIMPLE_SETTING_PATHS.has(path);
}
function refreshSetupAssistant() {
  const target = document.getElementById('setup-assistant-root');
  if (!target || !window.VectorWarpSetupAssistant || !activeConfig) return;
  window.VectorWarpSetupAssistant.render(target, {
    getConfig: () => activeConfig, mode: () => settingsMode,
    setMode(mode) {
      settingsMode = mode === 'advanced' ? 'advanced' : 'simple';
      try { window.localStorage.setItem('vectorwarp-settings-mode', settingsMode); } catch (_) {}
      renderEditor();
    },
    openTab(tab) { activateSettingsTab(tab); document.getElementById('config-fields')?.scrollIntoView({behavior: 'smooth'}); },
    set: setValue, refresh: renderEditor, refreshEditor: renderEditor
  });
}

function siteValue(site) {
  if (!site || typeof site.name !== 'string' || !site.name.trim() ||
      /^Set (receiver|transmitter) site$/.test(site.name) ||
      !['latitude', 'longitude', 'altitude'].every(key =>
        typeof site[key] === 'number' && Number.isFinite(site[key])) ||
      Math.abs(site.latitude) > 90 || Math.abs(site.longitude) > 180) return null;
  return {name: site.name, latitude: site.latitude,
    longitude: site.longitude, altitude: site.altitude};
}

function savedSiteChoices() {
  let stored = [];
  try {
    const raw = window.localStorage.getItem('blah2-recent-sites');
    if (raw && raw.length < 65536) stored = JSON.parse(raw);
  } catch (_) { /* Browser storage is optional. Current config still works. */ }
  const unique = values => [...new Map(values.map(siteValue).filter(Boolean)
    .map(site => [serializeConfig(site), site])).values()];
  const current = unique(Object.values(activeConfig.location || {}));
  const recent = unique([...recentSites, ...(Array.isArray(stored) ? stored : [])])
    .filter(site => !current.some(item => serializeConfig(item) === serializeConfig(site)))
    .slice(0, 32);
  return {current, recent};
}

function rememberSites() {
  const {current, recent} = savedSiteChoices();
  try { window.localStorage.setItem('blah2-recent-sites', JSON.stringify([...current, ...recent].slice(0, 32))); }
  catch (_) { /* Saving radar settings does not depend on browser storage. */ }
}

function useSite(role, site) {
  const previous = siteValue(activeConfig.location[role]);
  recentSites = [site, ...(previous ? [previous] : []), ...recentSites].slice(0, 32);
  setValue(['location', role], {...activeConfig.location[role], ...site});
  renderEditor();
}

function openSiteDialog(role, editing = false) {
  if (!capabilities.editable || saveInProgress || restartInProgress) return;
  const old = siteValue(activeConfig.location[role]);
  const initial = editing ? activeConfig.location[role] : null;
  document.getElementById('site-dialog')?.remove();
  const dialog = document.createElement('dialog');
  dialog.id = 'site-dialog';
  dialog.className = 'site-dialog';
  dialog.setAttribute('aria-labelledby', 'site-dialog-title');
  const form = document.createElement('form');
  const title = document.createElement('h2');
  title.id = 'site-dialog-title';
  title.textContent = `${editing ? 'Edit' : 'Add'} ${role === 'rx' ? 'receiver' : 'transmitter'} location`;
  form.appendChild(title);
  const controls = {};
  const inputs = document.createElement('div');
  inputs.className = 'site-dialog-fields';
  for (const [key, text] of [['name', 'Name'], ['latitude', 'Latitude'],
    ['longitude', 'Longitude'], ['altitude', 'Elevation (m)']]) {
    const label = document.createElement('label');
    const caption = document.createElement('span');
    caption.textContent = text;
    const input = document.createElement('input');
    input.name = key;
    input.id = `site-input-${key}`;
    input.type = key === 'name' ? 'text' : 'number';
    input.required = true;
    if (key !== 'name') {
      input.step = 'any';
      const rule = capabilities.fieldRules?.[`location.${role}.${key}`] || {};
      if (rule.min !== undefined) input.min = rule.min;
      if (rule.max !== undefined) input.max = rule.max;
    }
    input.value = initial?.[key] ?? (key === 'altitude' ? 0 : '');
    input.addEventListener('input', () => input.setCustomValidity(''));
    label.htmlFor = input.id;
    label.append(caption, input);
    inputs.appendChild(label);
    controls[key] = input;
  }
  const actions = document.createElement('div');
  actions.className = 'site-dialog-actions';
  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'button-secondary';
  cancel.textContent = 'Cancel';
  const apply = document.createElement('button');
  apply.type = 'submit';
  apply.className = 'button-primary';
  apply.textContent = 'Use location';
  const close = () => {
    if (typeof dialog.close === 'function') dialog.close();
    dialog.remove();
    document.getElementById(`site-select-${role}`)?.focus();
  };
  cancel.addEventListener('click', close);
  dialog.addEventListener('cancel', event => { event.preventDefault(); close(); });
  dialog.addEventListener('keydown', event => {
    if (event.key === 'Escape') { event.preventDefault(); close(); }
  });
  form.addEventListener('submit', event => {
    event.preventDefault();
    controls.name.setCustomValidity(controls.name.value.trim() ? '' : 'Enter a location name.');
    if (!form.reportValidity()) return;
    const site = {name: controls.name.value.trim(), latitude: Number(controls.latitude.value),
      longitude: Number(controls.longitude.value), altitude: Number(controls.altitude.value)};
    if (!siteValue(site)) return;
    if (old) recentSites.unshift(old);
    useSite(role, site);
    close();
  });
  actions.append(cancel, apply);
  form.append(inputs, actions);
  dialog.appendChild(form);
  document.body.appendChild(dialog);
  if (typeof dialog.showModal === 'function') dialog.showModal();
  else dialog.setAttribute('open', '');
  controls.name.focus();
}

function renderSiteSettings() {
  const section = document.createElement('div');
  section.className = 'site-settings';
  section.dataset.configGroup = 'location';
  const content = document.createElement('div');
  content.className = 'config-fields';
  const choices = savedSiteChoices();
  for (const [role, title] of [['rx', 'Receiver'], ['tx', 'Transmitter']]) {
    const row = document.createElement('div');
    row.className = 'config-field site-setting';
    row.dataset.path = `location.${role}`;
    const copy = document.createElement('div');
    copy.className = 'config-field-copy';
    const label = document.createElement('label');
    label.textContent = title;
    label.htmlFor = `site-select-${role}`;
    const rawSite = activeConfig.location[role];
    const current = siteValue(rawSite);
    const named = typeof rawSite?.name === 'string' && rawSite.name.trim() &&
      !/^Set (receiver|transmitter) site$/.test(rawSite.name);
    const coordinates = document.createElement('small');
    coordinates.textContent = current ? `${current.latitude}°, ${current.longitude}° · ${current.altitude} m` :
      named ? 'Check coordinates' : 'Not set';
    copy.append(label, coordinates);
    const controls = document.createElement('div');
    controls.className = 'site-controls';
    const select = document.createElement('select');
    select.id = label.htmlFor;
    select.setAttribute('aria-label', `${title} location`);
    select.disabled = !capabilities.editable;
    select.dataset.originalDisabled = String(select.disabled);
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = !current && named ? rawSite.name : 'Choose a location';
    placeholder.disabled = true;
    placeholder.selected = !current;
    select.appendChild(placeholder);
    const all = [...choices.current, ...choices.recent];
    for (const [heading, sites] of [['Current config', choices.current], ['Saved on this browser', choices.recent]]) {
      if (!sites.length) continue;
      const group = document.createElement('optgroup');
      group.label = heading;
      for (const site of sites) {
        const option = document.createElement('option');
        option.value = all.indexOf(site);
        option.textContent = site.name;
        if (current && serializeConfig(current) === serializeConfig(site)) option.selected = true;
        group.appendChild(option);
      }
      select.appendChild(group);
    }
    select.addEventListener('change', () => { if (all[select.value]) useSite(role, all[select.value]); });
    const actions = document.createElement('div');
    actions.className = 'site-actions';
    for (const [action, text] of [['edit', 'Edit'], ['add', 'Add location']]) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'button-secondary';
      button.textContent = text;
      button.dataset.siteAction = action;
      button.dataset.siteRole = role;
      button.disabled = !capabilities.editable || (action === 'edit' && !current && !named);
      button.addEventListener('click', () => openSiteDialog(role, action === 'edit'));
      actions.appendChild(button);
    }
    controls.append(select, actions);
    const error = document.createElement('small');
    error.className = 'config-field-error';
    error.setAttribute('aria-live', 'polite');
    row.append(copy, controls, error);
    content.appendChild(row);
  }
  section.appendChild(content);
  return section;
}

async function configFetch(url, options = {}, timeoutMs = 6000) {
  return fetchStatusResource(url, options, timeoutMs);
}

function serializeConfig(value) {
  if (Array.isArray(value)) return `[${value.map(serializeConfig).join(',')}]`;
  if (value !== null && typeof value === 'object')
    return `{${Object.keys(value).sort().map(key => `${JSON.stringify(key)}:${serializeConfig(value[key])}`).join(',')}}`;
  return JSON.stringify(value);
}

const NUMERIC_PRESENTATION = {
  'capture.fs': [1000000, 'MS/s'],
  'capture.fc': [1000000, 'MHz'],
  'process.data.cpi': [1, 's'],
  'process.data.buffer': [1, 'intervals'],
  'process.data.overlap': [1, 's'],
  'process.ambiguity.delayMin': [1, 'bin'],
  'process.ambiguity.delayMax': [1, 'bin'],
  'process.ambiguity.dopplerMin': [1, 'Hz'],
  'process.ambiguity.dopplerMax': [1, 'Hz'],
  'process.clutter.delayMin': [1, 'bin'],
  'process.clutter.delayMax': [1, 'bin'],
  'process.detection.minDelay': [1, 'bin'],
  'process.detection.minDoppler': [1, 'Hz'],
  'process.tracker.initiate.maxAcc': [1, 'Hz/s'],
  'location.rx.latitude': [1, '°'],
  'location.rx.longitude': [1, '°'],
  'location.tx.latitude': [1, '°'],
  'location.tx.longitude': [1, '°'],
  'location.rx.altitude': [1, 'm'],
  'location.tx.altitude': [1, 'm']
};

function words(value) {
  return value.replace(/([a-z0-9])([A-Z])/g, '$1 $2')
    .replace(/_/g, ' ').replace(/^./, letter => letter.toUpperCase());
}

function metadata(path, receiverType = activeConfig?.capture?.device?.type) {
  const key = path.join('.');
  if (key === 'capture.fs') {
    if (receiverType === 'Kraken')
      return ['Sample rate', 'Kraken Suite currently streams at a fixed 2.4 MS/s.'];
    if (receiverType === 'RspDuo')
      return ['Sample rate', 'RSPduo dual-tuner output rate, from 62.5 kS/s to 2 MS/s.'];
    return ['Sample rate', 'Radio samples per second for the selected receiver.'];
  }
  if (key === 'capture.device.serial' && receiverType === 'RspDuo')
    return ['RSPduo serial', 'Optional exact selection; blank only with one matching receiver.'];
  if (CONFIG_META[key]) return CONFIG_META[key];
  const raw = path[path.length - 1];
  const label = words(raw);
  const section = path.length > 1 ? words(path[path.length - 2]) : 'System';
  const normalized = raw.toLowerCase();
  let description = `${section} setting`;
  if (normalized === 'enable' || normalized === 'enabled')
    description = `Turn ${section.toLowerCase()} on or off`;
  else if (normalized.includes('latitude')) description = 'Decimal degrees north';
  else if (normalized.includes('longitude')) description = 'Decimal degrees east';
  else if (normalized.endsWith('_hz')) description = `${section} frequency in hertz`;
  else if (normalized.endsWith('_db')) description = `${section} level in decibels`;
  else if (normalized.endsWith('_deg') || normalized.endsWith('_degrees'))
    description = `${section} angle in degrees`;
  else if (normalized.endsWith('_ms')) description = `${section} duration in milliseconds`;
  else if (normalized.includes('_per_second')) description = `${section} rate per second`;
  else if (normalized.endsWith('_bins')) description = `${section} distance in map bins`;
  else if (normalized.endsWith('_frames')) description = `${section} frame count`;
  else if (normalized.endsWith('_samples')) description = `${section} sample count`;
  else if (normalized.endsWith('_interval')) description = `Frames between ${section.toLowerCase()} updates`;
  else if (normalized.endsWith('_threshold')) description = `${section} activation threshold`;
  else if (normalized.endsWith('_ratio')) description = `${section} ratio limit`;
  else if (normalized.endsWith('_path')) description = `File path used by ${section.toLowerCase()}`;
  else if (normalized.includes('loading')) description = `${section} numerical regularization`;
  else if (normalized.includes('alpha') || normalized === 'decay')
    description = `${section} smoothing factor from 0 to 1`;
  return [label, description];
}

function setValue(path, value) {
  const gainPolicy = path.join('.') === 'capture.device.heimdall.gain_control'
    ? value : activeConfig.capture?.device?.heimdall?.gain_control;
  const geometryOptions = {ignoreGain: gainPolicy === 'measured'};
  const geometryContext = geometryEditor?.context(activeConfig, geometryOptions);
  let target = activeConfig;
  path.slice(0, -1).forEach(key => { target = target[key]; });
  target[path[path.length - 1]] = value;
  if (path.join('.') === 'capture.fc' && activeConfig.capture?.device?.type === 'RspDuo') {
    const control = document.getElementById('config-capture-device-lnaState-0');
    if (control) {
      const next = rspDuoLnaInput(activeConfig.capture.device.lnaState);
      control.replaceChildren(...Array.from(next.childNodes));
    }
  }
  if (geometryEditor && geometryContext !== geometryEditor.context(activeConfig, geometryOptions)) {
    geometryEditor.invalidate(activeConfig.capture?.device?.array_geometry);
    refreshGeometryEditor();
  }
  scheduleValidation();
}

function normalizeKrakenChannels(config, previousCount = null) {
  if (config.capture?.device?.type !== 'Kraken') return config;
  const device = config.capture.device;
  geometryEditor?.invalidate(device.array_geometry);
  const synthesis = config.process?.reference_synthesis;
  const count = device.channel_count;
  if (!Number.isInteger(count) || count < 2 || count > 8 || !synthesis)
    return config;
  if (!Number.isInteger(device.reference_channel) ||
      device.reference_channel < 0 || device.reference_channel >= count)
    device.reference_channel = 0;
  const channels = Array.from({length: count}, (_, index) => index);
  const prune = list => [...new Set((list || []).filter(channel =>
    Number.isInteger(channel) && channel >= 0 && channel < count))];
  const expandDefault = list => previousCount && list?.length === previousCount &&
    list.every((channel, index) => channel === index) ? channels : prune(list);
  device.surveillance_channels = expandDefault(device.surveillance_channels);
  if (synthesis.mode === 'dedicated') {
    synthesis.channels = [device.reference_channel];
    device.surveillance_channels = device.surveillance_channels.filter(channel =>
      channel !== device.reference_channel);
    if (!device.surveillance_channels.length)
      device.surveillance_channels = channels.filter(channel => channel !== device.reference_channel);
  } else {
    synthesis.channels = expandDefault(synthesis.channels);
    if (synthesis.channels.length < 2) synthesis.channels = channels;
  }
  return config;
}

function applyDeviceProfile(config, profile) {
  const geometry = config.capture?.device?.array_geometry;
  const acceleration = config.process?.performance?.acceleration ?? 'auto';
  config.capture.device = JSON.parse(JSON.stringify(profile.device));
  if (geometry !== undefined) config.capture.device.array_geometry = geometryEditor ?
    geometryEditor.invalidate(geometry) : geometry;
  config.capture.fs = profile.sampleRate;
  const merge = (target, source) => Object.entries(source || {}).reduce((result, [key, value]) => {
    result[key] = value && typeof value === 'object' && !Array.isArray(value) ?
      merge(result[key] && typeof result[key] === 'object' ? result[key] : {}, value) :
      JSON.parse(JSON.stringify(value));
    return result;
  }, target);
  for (const [key, value] of Object.entries(profile.process || {})) {
    if (key === 'performance') config.process.performance = merge(config.process.performance || {}, value);
    else config.process[key] = merge(config.process[key] || {}, value);
  }
  if (!profile.process?.reference_synthesis) delete config.process.reference_synthesis;
  if (!profile.features?.coherentArray) {
    delete config.process.surveillance_fusion;
    if (config.process.bearing) {
      config.process.bearing.enable = false;
      if (config.process.bearing.frequency_resolved_shadow)
        config.process.bearing.frequency_resolved_shadow.enabled = false;
      if (config.process.bearing.manifold_recorder)
        config.process.bearing.manifold_recorder.enabled = false;
    }
    if (config.process.detection) {
      config.process.detection.integration_cpis = 1;
      config.process.detection.integration_motion_bins = 0;
      if (config.process.detection.track_before_detect)
        config.process.detection.track_before_detect.enable = false;
    }
    if (config.process.tracker) {
      delete config.process.tracker.gpu_capacity;
      delete config.process.tracker.beam_tolerance;
      if (config.process.tracker.track_guided_shadow) {
        config.process.tracker.track_guided_shadow.enable = false;
        config.process.tracker.track_guided_shadow.promote = false;
      }
      if (config.process.tracker.initiate?.stationary_clutter)
        config.process.tracker.initiate.stationary_clutter.enable = false;
    }
  }
  config.process.performance = {...config.process.performance, acceleration};
  return config;
}

function receiverFeatureCapabilities(type, profiles = []) {
  return profiles.find(profile => profile.type === type)?.features || {};
}

function receiverSpecificProcessPath(path) {
  return path === 'process.reference_synthesis' || path.startsWith('process.reference_synthesis.') ||
    path === 'process.surveillance_fusion' || path.startsWith('process.surveillance_fusion.') ||
    path === 'process.bearing' || path.startsWith('process.bearing.') ||
    path === 'process.detection.integration_cpis' ||
    path === 'process.detection.integration_motion_bins' ||
    path === 'process.detection.track_before_detect' ||
    path.startsWith('process.detection.track_before_detect.') ||
    path === 'process.tracker.gpu_capacity' || path === 'process.tracker.beam_tolerance' ||
    path === 'process.tracker.track_guided_shadow' ||
    path.startsWith('process.tracker.track_guided_shadow.') ||
    path === 'process.tracker.initiate.stationary_clutter' ||
    path.startsWith('process.tracker.initiate.stationary_clutter.');
}

function switchDevice(type) {
  const profile = (capabilities.deviceProfiles || [])
    .find(item => item.type === type);
  if (!profile) {
    showMessage(`${type} is not supported by this VectorWarp server.`, 'error');
    return;
  }
  applyDeviceProfile(activeConfig, profile);
  validationErrors = [];
  showMessage(profile.liveAvailable === false ?
    (profile.type === 'RspDuo' ?
      `${profile.label} selected. Install the SDRplay API, then use Receiver setup to build its live adapter before saving.` :
      `${profile.label} selected. Install its receiver software in Receiver setup before saving.`) :
    `${profile.label} defaults loaded. Review its hardware settings before saving.`, 'warning');
  renderEditor();
  scheduleValidation();
}

function selectInput(options, value, onChange) {
  const select = document.createElement('select');
  options.forEach(item => {
    const normalized = typeof item === 'object' ? item :
      {value: item, label: String(item)};
    const option = document.createElement('option');
    option.value = normalized.value;
    option.textContent = normalized.label;
    option.selected = String(normalized.value) === String(value);
    select.appendChild(option);
  });
  if (!Array.from(select.options).some(option => option.value === String(value))) {
    const invalid = document.createElement('option');
    invalid.value = String(value);
    invalid.textContent = `${value} (current value — review)`;
    invalid.selected = true;
    select.appendChild(invalid);
  }
  select.addEventListener('change', () => onChange(select.value));
  return select;
}

function rspDuoLnaInput(value) {
  const rule = capabilities.fieldRules?.['capture.device.lnaState'] || {};
  const band = rule.frequencyBands?.find(item => activeConfig.capture.fc < item.belowHz);
  const choices = (rule.choices || [value]).filter(item => !band || item <= band.max);
  return selectInput(choices, value, selected =>
    setValue(['capture', 'device', 'lnaState'], Number(selected)));
}

function frequencyInput(value, path) {
  const wrapper = document.createElement('div');
  wrapper.className = 'frequency-input';
  const whole = Math.floor(value / 1000000);
  const khz = Math.floor((value % 1000000) / 1000);
  const hz = Math.floor(value % 1000);
  const parts = [
    ['MHz', whole, 0, 4294],
    ['kHz', khz, 0, 999],
    ['Hz', hz, 0, 999]
  ];
  const inputs = [];
  const update = () => setValue(path, inputs.every(input => input.checkValidity()) ?
    Number(inputs[0].value) * 1000000 +
    Number(inputs[1].value) * 1000 + Number(inputs[2].value) : null);
  parts.forEach(([unit, partValue, min, max]) => {
    const label = document.createElement('label');
    const input = document.createElement('input');
    const suffix = document.createElement('span');
    input.type = 'number';
    input.required = true;
    input.inputMode = 'numeric';
    input.min = min;
    input.max = max;
    input.step = 1;
    input.value = partValue;
    input.setAttribute('aria-label', `Center frequency ${unit}`);
    suffix.textContent = unit;
    input.addEventListener('input', update);
    inputs.push(input);
    label.append(input, suffix);
    wrapper.appendChild(label);
  });
  return wrapper;
}

function arrayInput(value, path) {
  const key = path.join('.');
  const wrapper = document.createElement('div');
  const channelList = key === 'capture.device.surveillance_channels' ||
    key === 'process.reference_synthesis.channels';
  wrapper.className = channelList ? 'channel-choice-grid' : 'array-inputs';
  if (channelList) {
    const count = activeConfig.capture?.device?.channel_count || value.length;
    const dedicatedReference = key === 'process.reference_synthesis.channels' &&
      activeConfig.process?.reference_synthesis?.mode === 'dedicated';
    for (let channel = 0; channel < count; channel += 1) {
      const label = document.createElement('label');
      const input = document.createElement('input');
      input.type = 'checkbox';
      input.checked = value.includes(channel);
      input.disabled = dedicatedReference;
      input.addEventListener('change', () => {
        const selected = Array.from(wrapper.querySelectorAll('input:checked'))
          .map(item => Number(item.value));
        setValue(path, selected);
      });
      input.value = channel;
      label.append(input, document.createTextNode(`Ch ${channel}`));
      wrapper.appendChild(label);
    }
    return wrapper;
  }
  value.forEach((item, index) => {
    const slot = document.createElement('label');
    const caption = document.createElement('span');
    caption.textContent = value.length === 2 ?
      (index === 0 ? 'Reference' : 'Surveillance') : `Item ${index + 1}`;
    let input;
    const update = next => {
      const changed = [...value];
      changed[index] = next;
      value = changed;
      setValue(path, changed);
    };
    if (typeof item === 'boolean') {
      input = selectInput([
        {value: 'false', label: 'Off'}, {value: 'true', label: 'On'}
      ], String(item), selected => update(selected === 'true'));
    } else if (key === 'capture.device.gain_lna') {
      input = selectInput([0, 8, 16, 24, 32, 40], item,
        selected => update(Number(selected)));
    } else if (key === 'capture.device.gain_vga') {
      input = selectInput(Array.from({length: 32}, (_, i) => i * 2), item,
        selected => update(Number(selected)));
    } else if (key === 'capture.device.gainReduction') {
      input = selectInput(Array.from({length: 40}, (_, i) => i + 20), item,
        selected => update(Number(selected)));
    } else {
      input = document.createElement('input');
      input.type = typeof item === 'number' ? 'number' : 'text';
      if (typeof item === 'number') input.step = 'any';
      input.value = item;
      input.addEventListener('input', () => update(typeof item === 'number' ?
        (input.value === '' ? null : Number(input.value)) : input.value));
    }
    slot.append(caption, input);
    wrapper.appendChild(slot);
  });
  return wrapper;
}

const ADSB_PUBLIC_EXCHANGES = [
  {id: 'adsb-fi', label: 'adsb.fi', host: 'opendata.adsb.fi',
    description: 'Public exchange · best effort and rate limited'},
  {id: 'adsb-lol', label: 'adsb.lol', host: 'api.adsb.lol',
    description: 'Public exchange · best effort and rate limited'}
];

function adsbExchangeId(source) {
  if (typeof source !== 'string' || !source.startsWith('exchange:')) return null;
  try {
    const host = new URL(source.slice('exchange:'.length)).hostname;
    return ADSB_PUBLIC_EXCHANGES.find(exchange => exchange.host === host)?.id || null;
  } catch (_) { return null; }
}

function adsbPublicSource(id, config = activeConfig) {
  const latitude = Number(config.location?.rx?.latitude);
  const longitude = Number(config.location?.rx?.longitude);
  if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) return null;
  if (id === 'adsb-fi')
    return `exchange:https://opendata.adsb.fi/api/v3/lat/${latitude}/lon/${longitude}/dist/60`;
  if (id === 'adsb-lol')
    return `exchange:https://api.adsb.lol/v2/point/${latitude}/${longitude}/60`;
  return null;
}

function adsbSourcesInput(legacyValue, path) {
  const configured = activeConfig.truth?.adsb?.sources;
  let sources = Array.isArray(configured) && configured.length ?
    configured.map(value => String(value)) : [String(legacyValue ?? 'auto')];
  const wrapper = document.createElement('div');
  wrapper.className = 'adsb-source-editor';

  const writeSources = next => {
    sources = [...new Set(next)].slice(0, 8);
    const firstLocal = sources.find(source => !source.startsWith('exchange:'));
    if (firstLocal) activeConfig.truth.adsb.tar1090 = firstLocal;
    setValue(['truth', 'adsb', 'sources'], sources);
  };

  const commit = () => {
    const next = [...wrapper.querySelectorAll('[data-adsb-source]')]
      .map(input => input.value.trim()).filter(Boolean);
    for (const exchange of ADSB_PUBLIC_EXCHANGES) {
      const toggle = wrapper.querySelector(`[data-adsb-exchange="${exchange.id}"]`);
      if (!toggle?.checked) continue;
      const existing = sources.find(source => adsbExchangeId(source) === exchange.id);
      const generated = adsbPublicSource(exchange.id);
      if (existing || generated) next.push(existing || generated);
    }
    writeSources(next);
    for (const exchange of ADSB_PUBLIC_EXCHANGES) {
      const toggle = wrapper.querySelector(`[data-adsb-exchange="${exchange.id}"]`);
      if (!toggle) continue;
      toggle.checked = sources.some(source => adsbExchangeId(source) === exchange.id);
      toggle.disabled = !toggle.checked && sources.length >= 8;
    }
    const add = wrapper.querySelector('.adsb-source-add');
    if (add) add.disabled = sources.length >= 8;
  };

  const sourceRow = (source, type) => {
    const row = document.createElement('div');
    row.className = 'adsb-source-row';
    const input = document.createElement('input');
    input.type = 'text';
    input.value = source;
    input.dataset.adsbSource = type;
    input.placeholder = type === 'local' ?
      'http://192.168.1.50/tar1090' : 'exchange:https://example/api';
    input.setAttribute('aria-label', type === 'local' ? 'Local ADS-B source' : 'Custom ADS-B exchange');
    input.addEventListener('input', () => {
      const index = Number(input.dataset.sourceIndex);
      sources[index] = input.value;
      const next = sources.filter(Boolean);
      const firstLocal = next.find(value => !value.startsWith('exchange:'));
      if (firstLocal) activeConfig.truth.adsb.tar1090 = firstLocal;
      setValue(path, next);
    });
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'button-secondary adsb-source-remove';
    remove.textContent = 'Remove';
    remove.setAttribute('aria-label', `Remove ${type} ADS-B source`);
    remove.disabled = sources.length <= 1;
    remove.addEventListener('click', () => {
      sources.splice(Number(input.dataset.sourceIndex), 1);
      writeSources(sources);
      renderRows();
    });
    row.append(input, remove);
    return {row, input};
  };

  const renderRows = () => {
    wrapper.replaceChildren();
    const intro = document.createElement('p');
    intro.className = 'adsb-source-note';
    intro.textContent = 'One aircraft is shown once. The newest healthy observation wins; positions are not averaged.';
    wrapper.appendChild(intro);

    const timingWarning = document.createElement('p');
    timingWarning.className = 'adsb-source-note warning';
    timingWarning.textContent = 'Using only local ADS-B can add feed and host-arrival timing uncertainty to pass statistics. Add an online source for independent coverage and observation-time cross-checking; exact radar event time is still required for decision-grade scoring.';
    wrapper.appendChild(timingWarning);

    const localHeading = document.createElement('strong');
    localHeading.textContent = 'Local receivers';
    wrapper.appendChild(localHeading);
    sources.forEach((source, index) => {
      if (source.startsWith('exchange:')) return;
      const {row, input} = sourceRow(source, 'local');
      input.dataset.sourceIndex = index;
      wrapper.appendChild(row);
    });
    const addLocal = document.createElement('button');
    addLocal.type = 'button';
    addLocal.className = 'button-secondary adsb-source-add';
    addLocal.textContent = 'Add local source';
    addLocal.disabled = sources.length >= 8;
    addLocal.addEventListener('click', () => {
      sources.push('http://');
      writeSources(sources);
      renderRows();
      const inputs = wrapper.querySelectorAll('[data-adsb-source="local"]');
      inputs[inputs.length - 1]?.focus();
    });
    wrapper.appendChild(addLocal);

    const exchangeHeading = document.createElement('strong');
    exchangeHeading.textContent = 'Online sources';
    wrapper.appendChild(exchangeHeading);
    const hasSite = ADSB_PUBLIC_EXCHANGES.every(exchange => adsbPublicSource(exchange.id));
    for (const exchange of ADSB_PUBLIC_EXCHANGES) {
      const row = document.createElement('div');
      row.className = 'adsb-exchange-row';
      const copy = document.createElement('span');
      const title = document.createElement('b');
      title.textContent = exchange.label;
      const detail = document.createElement('small');
      detail.textContent = exchange.description;
      copy.append(title, detail);
      const label = document.createElement('label');
      label.className = 'switch';
      const input = document.createElement('input');
      input.type = 'checkbox';
      input.dataset.adsbExchange = exchange.id;
      input.checked = sources.some(source => adsbExchangeId(source) === exchange.id);
      input.disabled = !hasSite || (!input.checked && sources.length >= 8);
      input.setAttribute('aria-label', `Use ${exchange.label}`);
      input.addEventListener('change', commit);
      label.append(input, document.createElement('span'));
      row.append(copy, label);
      wrapper.appendChild(row);
    }

    const photoHeading = document.createElement('strong');
    photoHeading.textContent = 'Aircraft photos';
    wrapper.appendChild(photoHeading);
    const photoRow = document.createElement('div');
    photoRow.className = 'adsb-exchange-row';
    const photoCopy = document.createElement('span');
    const photoTitle = document.createElement('b');
    photoTitle.textContent = 'Selected-aircraft photo';
    const photoDetail = document.createElement('small');
    photoDetail.textContent = 'Loaded only after a map selection; never blocks or enlarges the live ADS-B feed.';
    photoCopy.append(photoTitle, photoDetail);
    const photoSelect = document.createElement('select');
    photoSelect.dataset.adsbPhotoProvider = 'true';
    photoSelect.setAttribute('aria-label', 'Aircraft photo provider');
    for (const [value, label] of [['disabled', 'Off (private and fastest)'],
      ['planespotters', 'Planespotters.net thumbnails']]) {
      const option = document.createElement('option');
      option.value = value; option.textContent = label;
      photoSelect.appendChild(option);
    }
    photoSelect.value = activeConfig.truth?.adsb?.aircraft_photo_provider || 'disabled';
    photoSelect.addEventListener('change', () =>
      setValue(['truth', 'adsb', 'aircraft_photo_provider'], photoSelect.value));
    photoRow.append(photoCopy, photoSelect);
    wrapper.appendChild(photoRow);
    const photoNote = document.createElement('p');
    photoNote.className = 'adsb-source-note';
    photoNote.textContent = 'Planespotters receives the selected aircraft ICAO address. Photos include photographer credit and a link to the original.';
    wrapper.appendChild(photoNote);

    const custom = sources.map((source, index) => ({source, index}))
      .filter(({source}) => source.startsWith('exchange:') && !adsbExchangeId(source));
    if (custom.length) {
      const customHeading = document.createElement('strong');
      customHeading.textContent = 'Custom online sources';
      wrapper.appendChild(customHeading);
      for (const {source, index} of custom) {
        const {row, input} = sourceRow(source, 'custom');
        input.dataset.sourceIndex = index;
        wrapper.appendChild(row);
      }
    }
    if (!hasSite) {
      const warning = document.createElement('p');
      warning.className = 'adsb-source-note warning';
      warning.textContent = 'Set the receiver latitude and longitude before enabling an online source.';
      wrapper.appendChild(warning);
    }
    const controls = wrapper.querySelectorAll('input, select');
    for (const [index, control] of Array.from(controls).entries()) {
      control.id = `config-truth-adsb-tar1090-${index}`;
      control.setAttribute('aria-describedby', 'config-truth-adsb-tar1090-0-help');
    }
  };

  renderRows();
  return wrapper;
}

function typedInput(value, path) {
  const key = path.join('.');
  const rule = capabilities.fieldRules?.[key] || {};
  if (key === 'truth.adsb.tar1090' && Array.isArray(rule.sourceChoices)) {
    return adsbSourcesInput(value, ['truth', 'adsb', 'sources']);
  }
  if (key === 'capture.device.heimdall.gain') {
    const wrapper = document.createElement('div');
    wrapper.className = 'config-suite-gain';
    const isManual = typeof value === 'number' && value !== -1;
    const known = value === 'keep' || value === -1 || isManual;
    const manual = document.createElement('input');
    manual.type = 'number'; manual.min = '0'; manual.max = '50'; manual.step = '0.1';
    manual.value = isManual ? String(value) : '';
    manual.hidden = !isManual;
    manual.disabled = !isManual;
    manual.setAttribute('aria-label', 'Manual Suite gain in dB');
    const mode = selectInput([
      {value: 'keep', label: 'Keep current receiver gain'},
      {value: '-1', label: 'Suite AGC (unmeasured)'},
      {value: 'manual', label: 'Manual gain'}
    ], known ? (isManual ? 'manual' : String(value)) : String(value), selected => {
      manual.hidden = selected !== 'manual';
      manual.disabled = selected !== 'manual';
      manual.dataset.originalDisabled = String(manual.disabled);
      if (selected === 'keep') setValue(path, 'keep');
      else if (selected === '-1') setValue(path, -1);
      else setValue(path, manual.value === '' ? null : Number(manual.value));
    });
    mode.setAttribute('aria-label', 'Suite gain mode');
    manual.addEventListener('input', () => {
      if (mode.value === 'manual') setValue(path, manual.value === '' ? null : Number(manual.value));
    });
    wrapper.append(mode, manual);
    return wrapper;
  }
  if (rule.type === 'autoBoolean') {
    const selected = typeof value === 'boolean' ? (value ? 'on' : 'off') : value;
    return selectInput(rule.choices, selected, next => setValue(path, next));
  }
  if (typeof value === 'boolean' || rule.type === 'boolean') {
    const label = document.createElement('label');
    label.className = 'switch';
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = value;
    const slider = document.createElement('span');
    input.addEventListener('change', () => setValue(path, input.checked));
    label.append(input, slider);
    return label;
  }
  if (key === 'capture.fc') return frequencyInput(value, path);
  if (key === 'capture.device.heimdall.gain_control')
    return selectInput([{value: 'measured', label: 'Automatic headroom check'},
      {value: 'fixed', label: 'Fixed gain'}], value, selected => setValue(path, selected));
  if (key === 'capture.device.lnaState') return rspDuoLnaInput(value);
  if (key === 'process.performance.acceleration')
    return selectInput([{value: 'auto', label: 'Automatic'}, {value: 'cpu', label: 'CPU'},
      {value: 'gpu', label: 'GPU (with CPU fallback)'}], value,
      selected => setValue(path, selected));
  if (Array.isArray(value)) return arrayInput(value, path);
  if (rule.choices && !['capture.device.type',
    'process.reference_synthesis.mode', 'capture.device.channel_count',
    'capture.device.bandwidthNumber'].includes(key))
    return selectInput(rule.choices, value, selected =>
      setValue(path, rule.type === 'number' ? Number(selected) : selected));
  if (key === 'capture.device.type' || key === 'process.reference_synthesis.mode' ||
      key === 'capture.device.bandwidthNumber' ||
      key === 'process.tracker.smooth' ||
      key === 'capture.device.channel_count' ||
      key === 'capture.device.reference_channel' ||
      key === 'process.performance.surveillance_workers' ||
      key === 'process.performance.fft_threads') {
    const deviceProfiles = capabilities.deviceProfiles || [];
    const options = key === 'capture.device.type' ?
      (deviceProfiles.length ? deviceProfiles.map(profile => profile.type) :
        [String(value)]) :
      key === 'process.reference_synthesis.mode' ?
        ['array_eigenbeam', 'dedicated'] :
      key === 'capture.device.bandwidthNumber' ?
        [0, 5, 50, 100] :
      key === 'capture.device.channel_count' ?
        [2, 3, 4, 5, 6, 7, 8] :
      key === 'capture.device.reference_channel' ?
        Array.from({length: activeConfig.capture.device.channel_count},
          (_, index) => index) :
      key === 'process.performance.surveillance_workers' ?
        Array.from({length: activeConfig.capture.device.type === 'Kraken' ?
          activeConfig.capture.device.channel_count + 1 : 2},
          (_, index) => index) :
      key === 'process.performance.fft_threads' ?
        [0, 1, 2, 4, 8, 16, 32, 64] : ['none'];
    const select = document.createElement('select');
    const optionValues = [...options];
    if (!optionValues.some(optionValue => String(optionValue) === String(value)))
      optionValues.unshift(value);
    optionValues.forEach(optionValue => {
      const option = document.createElement('option');
      option.value = optionValue;
      const profile = deviceProfiles.find(item => item.type === optionValue);
      option.textContent = key === 'capture.device.bandwidthNumber' ?
        (optionValue === 0 ? 'Off' : `${optionValue} Hz`) :
        key === 'process.reference_synthesis.mode' ?
          (optionValue === 'array_eigenbeam' ? 'Combine inputs (synthesized)' :
            optionValue === 'dedicated' ? 'Use one dedicated input' : String(optionValue)) :
        ['process.performance.surveillance_workers', 'process.performance.fft_threads'].includes(key) && optionValue === 0 ?
          'Auto' :
        (profile ? profile.label :
          key === 'capture.device.type' && optionValue === 'Unconfigured' ?
            'Choose a receiver…' : words(String(optionValue)));
      option.selected = String(optionValue) === String(value);
      select.appendChild(option);
    });
    select.addEventListener('change', () => {
      if (key === 'capture.device.type') switchDevice(select.value);
      else if (key === 'capture.device.bandwidthNumber')
        setValue(path, Number(select.value));
      else if (['capture.device.channel_count',
        'capture.device.reference_channel',
        'process.performance.surveillance_workers',
        'process.performance.fft_threads'].includes(key)) {
        const previousCount = activeConfig.capture.device.channel_count;
        setValue(path, Number(select.value));
        if (key.startsWith('capture.device.')) {
          normalizeKrakenChannels(activeConfig, previousCount);
          renderEditor();
        }
      }
      else {
        setValue(path, select.value);
        if (key === 'process.reference_synthesis.mode') {
          normalizeKrakenChannels(activeConfig);
          renderEditor();
        }
      }
    });
    return select;
  }
  if (key === 'capture.fs' &&
      ['RspDuo', 'Kraken'].includes(activeConfig.capture?.device?.type)) {
    const sampleRates = activeConfig.capture.device.type === 'Kraken' ?
      [2400000] : [62500, 125000, 250000, 500000, 1000000, 2000000];
    return selectInput(sampleRates.map(rate => ({value: rate, label: rate >= 1000000 ?
      `${rate / 1000000} MS/s` : `${rate / 1000} kS/s`})), value,
    selected => setValue(path, Number(selected)));
  }
  const input = document.createElement('input');
  if (typeof value === 'number' || rule.type === 'number') {
    const presentation = NUMERIC_PRESENTATION[key];
    const scale = presentation?.[0] || 1;
    input.type = 'number';
    input.step = rule.integer ? 1 / scale : 'any';
    if (rule.min !== undefined) input.min = rule.min / scale;
    if (rule.max !== undefined) input.max = rule.max / scale;
    input.required = true;
    input.value = value / scale;
    input.addEventListener('input', () => {
      setValue(path, input.value === '' ? null : Number(input.value) * scale);
    });
    if (presentation) {
      const wrapper = document.createElement('div');
      wrapper.className = 'input-with-unit';
      const unit = document.createElement('span');
      unit.textContent = presentation[1];
      wrapper.append(input, unit);
      return wrapper;
    }
  } else {
    input.type = 'text';
    const optionalRspSerial = key === 'capture.device.serial' &&
      activeConfig.capture?.device?.type === 'RspDuo';
    input.required = !optionalRspSerial;
    if (optionalRspSerial) input.maxLength = 160;
    input.value = value ?? '';
    input.addEventListener('input', () => setValue(path, input.value));
  }
  return input;
}

function field(value, path) {
  const [labelText, description] = metadata(path);
  const wrapper = document.createElement('div');
  wrapper.className = 'config-field';
  wrapper.dataset.path = path.join('.');
  const copy = document.createElement('div');
  copy.className = 'config-field-copy';
  const label = document.createElement('label');
  label.textContent = labelText;
  const help = document.createElement('small');
  help.textContent = description;
  copy.append(label, help);
  const control = path.join('.') === 'save.path' ? recordingFolderControl(value, path) : typedInput(value, path);
  const rule = capabilities.fieldRules?.[path.join('.')];
  const unusedDedicatedInput = path.join('.') === 'capture.device.reference_channel' &&
    activeConfig.process?.reference_synthesis?.mode === 'array_eigenbeam';
  const readOnly = !rule || rule.readOnly || !capabilities.editable || unusedDedicatedInput;
  const inputs = control.matches('input, select') ? [control] : control.querySelectorAll('input, select');
  for (const [index, input] of Array.from(inputs).entries()) {
    input.id = `config-${path.join('-')}-${index}`;
    input.setAttribute('aria-describedby', `config-${path.join('-')}-0-help`);
    if (index === 0) { label.htmlFor = input.id; help.id = `${input.id}-help`; }
    if (!input.hasAttribute('aria-label')) input.setAttribute('aria-label', `${labelText}${inputs.length > 1 ? ` ${index + 1}` : ''}`);
    if (readOnly) input.disabled = true;
    input.dataset.originalDisabled = String(input.disabled);
  }
  if (readOnly) help.textContent = unusedDedicatedInput ?
    'Not used in combined-input mode. The reference is synthesized from the inputs below.' :
    `${description} ${rule?.readOnly || (!rule ? 'Unknown setting preserved; edit in YAML if needed.' : '')}`;
  const error = document.createElement('small');
  error.className = 'config-field-error';
  error.setAttribute('aria-live', 'polite');
  wrapper.append(copy, control, error);
  return wrapper;
}

function recordingFolderControl(value, path) {
  const wrapper = document.createElement('div');
  wrapper.className = 'recording-folder-control';
  const select = document.createElement('select');
  select.required = true;
  const current = document.createElement('option');
  current.value = value; current.textContent = value; current.selected = true;
  select.appendChild(current);
  select.addEventListener('change', () => setValue(path, select.value));
  const create = document.createElement('button');
  create.type = 'button'; create.className = 'button-secondary'; create.textContent = 'New folder';
  const load = async selected => {
    try {
      const response = await configFetch(liveApiUrl('/api/recordings/directories'), {cache: 'no-store'});
      if (!response.ok) throw new Error('Folder list unavailable');
      const result = await response.json();
      select.replaceChildren();
      for (const item of result.directories || []) {
        const option = document.createElement('option');
        option.value = item.path; option.textContent = item.path;
        option.selected = item.path === selected; select.appendChild(option);
      }
      if (![...select.options].some(option => option.value === selected)) {
        const unavailable = document.createElement('option');
        unavailable.value = selected; unavailable.textContent = `${selected} (not writable)`;
        unavailable.selected = true; unavailable.disabled = true; select.prepend(unavailable);
      }
      select.disabled = !capabilities.editable;
      select.dataset.originalDisabled = String(select.disabled);
    } catch (_) { select.disabled = true; select.dataset.originalDisabled = 'true'; }
  };
  create.addEventListener('click', async () => {
    const name = window.prompt('New folder name inside VectorWarp recording storage');
    if (!name) return;
    create.disabled = true;
    try {
      const response = await configFetch(liveApiUrl('/api/recordings/directories'), {method: 'POST',
        headers: {'Content-Type': 'application/json', 'X-VectorWarp-Intent': 'recording-directory-v1'},
        body: JSON.stringify({name})});
      const result = await response.json();
      if (!response.ok) throw new Error(result.error || 'Could not create folder');
      setValue(path, result.path); await load(result.path);
    } catch (error) { window.alert(error.message); }
    finally { create.disabled = false; }
  });
  wrapper.append(select, create); void load(value);
  return wrapper;
}

function group(key, value, path, topLevel = false, options = {}) {
  const currentPath = [...path, key];
  const dotted = currentPath.join('.');
  const kraken = activeConfig.capture?.device?.type === 'Kraken';
  let [title, description] = metadata(currentPath);
  if (options.groupMeta?.[dotted]) [title, description] = options.groupMeta[dotted];
  if (dotted === 'capture.device') {
    const profile = capabilities.deviceProfiles?.find(item => item.type === value.type);
    title = profile ? profile.label : value.type || title;
    description = 'Connection and receiver-specific settings';
  }
  const details = document.createElement('details');
  details.dataset.configGroup = dotted;
  details.className = topLevel ? 'config-group config-group-root' : 'config-group';
  details.open = topLevel || dotted === 'capture.device' || dotted === 'process.reference_synthesis';
  const summary = document.createElement('summary');
  const heading = document.createElement('span');
  const strong = document.createElement('strong');
  strong.textContent = title;
  const small = document.createElement('small');
  small.textContent = description;
  heading.append(strong, small);
  summary.append(heading);
  const content = document.createElement('div');
  content.className = 'config-fields';
  if (dotted === 'capture' && value.device?.type !== undefined)
    content.appendChild(field(value.device.type, ['capture', 'device', 'type']));
  const order = {
    capture: ['fc', 'fs', 'device', 'aircraft_clips', 'replay'],
    'capture.device': ['channel_count', 'heimdall', 'surveillance_channels'],
    process: ['data', 'ambiguity', 'clutter', 'detection', 'tracker', 'performance'],
    'process.reference_synthesis': ['mode', 'channels']
  }[dotted] || [];
  const entries = Object.entries(dotted === 'process' ?
    {...value, performance: value.performance || {}} : value).sort(([left], [right]) =>
    (order.includes(left) ? order.indexOf(left) : 100) -
    (order.includes(right) ? order.indexOf(right) : 100));
  const advancedReference = document.createElement('details');
  advancedReference.className = 'config-group';
  const advancedTitle = document.createElement('summary');
  advancedTitle.textContent = 'Reference tuning (advanced)';
  const advancedFields = document.createElement('div');
  advancedFields.className = 'config-fields';
  advancedReference.append(advancedTitle, advancedFields);
  entries.forEach(([childKey, childValue]) => {
    const childPath = [...currentPath, childKey];
    if (options.skipPath?.(childPath.join('.'))) return;
    // Keep compatibility keys in the draft/YAML, not as nonfunctional controls.
    if (capabilities.fieldRules?.[childPath.join('.')]?.readOnly) return;
    // Structured controls own their backing values; do not render a second
    // generic editor for the same setting.
    if (capabilities.fieldRules?.[childPath.join('.')]?.structuredControl) return;
    if (dotted === 'process.data' && childKey === 'buffer') return;
    if (dotted === 'capture.device' && childKey === 'type') return;
    if (dotted === 'capture.device' && childKey === 'array_geometry') return;
    if (kraken && dotted === 'capture.device' && childKey === 'reference_channel') return;
    if (kraken && dotted === 'process' && childKey === 'reference_synthesis') return;
    if (kraken && dotted === 'process.reference_synthesis' && !['mode', 'channels'].includes(childKey)) {
      advancedFields.appendChild(field(childValue, [...currentPath, childKey]));
      return;
    }
    if (childValue !== null && typeof childValue === 'object' &&
        !Array.isArray(childValue))
      {
        const childGroup = group(childKey, childValue, currentPath, false, options);
        if (childGroup.querySelector('.config-field')) content.appendChild(childGroup);
      }
    else
      content.appendChild(field(childValue, [...currentPath, childKey]));
    if (kraken && dotted === 'process.reference_synthesis' && childKey === 'mode')
      content.appendChild(field(activeConfig.capture.device.reference_channel,
        ['capture', 'device', 'reference_channel']));
  });
  // Older saved Kraken configurations omit this optional control. Showing its
  // default does not write or retune anything until the operator changes it.
  if (dotted === 'capture.device.heimdall' && value.gain === undefined)
    content.appendChild(field('keep', [...currentPath, 'gain']));
  if (dotted === 'capture.device.heimdall' && value.gain_control === undefined)
    content.appendChild(field('fixed', [...currentPath, 'gain_control']));
  if (advancedFields.children.length) content.appendChild(advancedReference);
  if (settingsMode === 'advanced' && dotted === 'process.performance' &&
      activeConfig.process?.data?.buffer !== undefined)
    content.appendChild(field(activeConfig.process.data.buffer, ['process', 'data', 'buffer']));
  if (settingsMode === 'advanced' && kraken && dotted === 'capture.device' &&
      activeConfig.process?.reference_synthesis)
    content.appendChild(group('reference_synthesis', activeConfig.process.reference_synthesis, ['process']));
  const savedGeometry = activeConfig.capture?.device?.array_geometry;
  const geometryNeedsReview = savedGeometry !== undefined && (!savedGeometry ||
    typeof savedGeometry !== 'object' || !['regular_polygon', 'custom'].includes(savedGeometry.shape));
  if ((settingsMode === 'advanced' || geometryNeedsReview) && dotted === 'capture.device' && geometryEditor &&
      (kraken || savedGeometry !== undefined))
    content.appendChild(makeGeometryEditor());
  details.append(summary, content);
  return details;
}

function renderReceiverFeatures() {
  const section = document.createElement('section');
  section.className = 'receiver-feature-section';
  const type = activeConfig.capture?.device?.type;
  const features = receiverFeatureCapabilities(type, capabilities.deviceProfiles || []);
  if (!features.coherentArray) {
    const note = document.createElement('p');
    note.className = 'config-capability-note';
    note.textContent = 'This receiver uses its reference and surveillance inputs directly. Kraken array controls are hidden.';
    section.appendChild(note);
    return section;
  }
  const heading = document.createElement('h3');
  heading.textContent = 'Kraken array processing';
  const copy = document.createElement('p');
  copy.textContent = 'Optional coherent combining, longer integration, bearing, and site-manifold tools for the selected Kraken array.';
  section.append(heading, copy);
  const featureOptions = {groupMeta: {
    'process.detection': ['Coherent integration', 'Combine consecutive radar intervals and search short motion paths.'],
    'process.tracker': ['Array recovery experiments', 'Bounded array-only recovery tests; normal tracking remains in Radar.']
  }};
  const process = activeConfig.process || {};
  if (process.surveillance_fusion)
    section.appendChild(group('surveillance_fusion', process.surveillance_fusion, ['process'], false,
      featureOptions));
  const detection = process.detection || {};
  const residentDetection = Object.fromEntries(['integration_cpis', 'integration_motion_bins',
    'track_before_detect'].filter(key => detection[key] !== undefined).map(key => [key, detection[key]]));
  if (Object.keys(residentDetection).length)
    section.appendChild(group('detection', residentDetection, ['process'], false, featureOptions));
  const tracker = process.tracker || {};
  const residentTracker = {};
  for (const key of ['gpu_capacity', 'beam_tolerance', 'track_guided_shadow'])
    if (tracker[key] !== undefined) residentTracker[key] = tracker[key];
  if (tracker.initiate?.stationary_clutter !== undefined)
    residentTracker.initiate = {stationary_clutter: tracker.initiate.stationary_clutter};
  if (Object.keys(residentTracker).length)
    section.appendChild(group('tracker', residentTracker, ['process'], false, featureOptions));
  if (process.bearing)
    section.appendChild(group('bearing', process.bearing, ['process'], false, featureOptions));
  return section;
}

function makeGeometryEditor() {
  if (receiverInvalidatedGeometry)
    geometryEditor?.invalidate(activeConfig.capture?.device?.array_geometry);
  return geometryEditor.render(document, activeConfig, rebuild => {
    if (rebuild) refreshGeometryEditor();
    scheduleValidation();
  }, () => Boolean(capabilities.editable && !saveInProgress && !restartInProgress));
}

function refreshGeometryEditor() {
  const old = document.querySelector('.kraken-geometry');
  if (old && geometryEditor) {
    if (activeConfig.capture?.device?.type === 'Kraken' || activeConfig.capture?.device?.array_geometry !== undefined)
      old.replaceWith(makeGeometryEditor());
    else old.remove();
  }
}

function updateDirtyState() {
  const dirty = serializeConfig(activeConfig) !== originalConfig;
  const action = document.getElementById('config-save');
  const later = document.getElementById('config-save-later');
  const reset = document.getElementById('config-reset');
  if (action) {
    action.disabled = saveInProgress || restartInProgress || (!dirty && !restartRetry && !receiverApplyPending && !capabilities.setupRequired) ||
      pendingLiveConfig !== null || !capabilities.editable || validationPending || validationErrors.length > 0;
    if (!saveInProgress && !restartInProgress) action.textContent = restartRetry ? 'Retry restart' :
      receiverApplyPending ? (capabilities.restartAvailable ? 'Apply & Restart' : 'Apply (manual restart)') :
      capabilities.restartAvailable ? 'Save & Restart' : 'Save (manual restart)';
  }
  if (later) later.disabled = saveInProgress || restartInProgress || (!dirty && !capabilities.setupRequired) ||
    pendingLiveConfig !== null || !capabilities.editable || validationPending || validationErrors.length > 0;
  if (reset) {
    reset.disabled = saveInProgress || restartInProgress || (!dirty && pendingLiveConfig === null);
    reset.textContent = pendingLiveConfig === null ? 'Discard changes' : 'Reload saved config';
  }
  const state = document.getElementById('config-state');
  if (state) state.textContent = pendingLiveConfig !== null ?
    'Saved settings changed' : dirty ? 'Unsaved changes' : receiverApplyPending ? 'Apply settings to confirm' : 'Matches saved file';
  window.onbeforeunload = dirty ? () => true : null;
}

async function validateActiveConfiguration() {
  const sequence = ++validationSequence;
  const snapshot = serializeConfig(activeConfig);
  const body = JSON.stringify(activeConfig);
  validationPending = true;
  updateDirtyState();
  try {
    const response = await configFetch(liveApiUrl('/api/config/validate'), {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body
    });
    const result = await response.json();
    if (sequence !== validationSequence || snapshot !== serializeConfig(activeConfig)) return false;
    geometryEditor?.showAssessment(document, result.arrayGeometry);
    const nativeErrors = Array.from(document.querySelectorAll('#config-fields input, #config-fields select'))
      .filter(input => !input.disabled && !input.checkValidity()).map(input =>
        `${input.closest('.config-field').dataset.path}: ${input.validationMessage}`);
    validationErrors = nativeErrors.concat(result.valid ? [] :
      (result.errors || ['The configuration is invalid.']));
    validatedSnapshot = validationErrors.length ? null : snapshot;
    displayFieldErrors(validationErrors);
    if (validationErrors.length) {
      const shown = validationErrors.slice(0, 3).map(humanizeError).join(' ');
      const remaining = validationErrors.length - 3;
      showMessage(`${shown}${remaining > 0 ? ` Plus ${remaining} more issue${remaining === 1 ? '' : 's'}.` : ''}`, 'error');
    } else if (serializeConfig(activeConfig) !== originalConfig) {
      showMessage('');
    }
    validationNotices = result.notices || [];
    displayValidationNotices();
    return validationErrors.length === 0;
  } catch (_) {
    if (sequence !== validationSequence) return false;
    validationErrors = ['The server could not validate these settings.'];
    showMessage(validationErrors[0], 'error');
    return false;
  } finally {
    if (sequence === validationSequence) {
      validationPending = false;
      updateDirtyState();
    }
  }
}

function scheduleValidation() {
  validationSequence += 1;
  validatedSnapshot = null;
  if (validationTimer) window.clearTimeout(validationTimer);
  validationErrors = [];
  validationNotices = [];
  displayValidationNotices();
  validationPending = true;
  updateDirtyState();
  validationTimer = window.setTimeout(validateActiveConfiguration, 350);
}

function humanizeError(message) {
  return String(message).replace(/(?:configuration\.)?((?:capture|process|network|truth|location|save)(?:\.[\w]+)+)/g,
    (_, key) => metadata(key.split('.'))[0]);
}

function displayValidationNotices() {
  document.querySelectorAll('.config-advice').forEach(note => note.remove());
  for (const notice of validationNotices) {
    const field = Array.from(document.querySelectorAll('.config-field'))
      .find(element => element.dataset.path === notice.field);
    const group = Array.from(document.querySelectorAll('[data-config-group]'))
      .find(element => element.dataset.configGroup === notice.field);
    if (!field && !group) continue;
    const note = document.createElement('small');
    note.className = 'config-advice';
    note.textContent = notice.message;
    if (field) field.appendChild(note);
    else group.querySelector('.config-fields').prepend(note);
  }
}

function displayFieldErrors(errors) {
  document.querySelectorAll('.config-field').forEach(field => {
    const messages = errors.filter(error => error.includes(field.dataset.path));
    field.classList.toggle('invalid', messages.length > 0);
    field.querySelector('.config-field-error').textContent = messages.map(humanizeError).join(' ');
    field.querySelectorAll('input, select').forEach(input => input.setAttribute('aria-invalid', String(messages.length > 0)));
    if (messages.length) {
      let parent = field.parentElement;
      while (parent) { if (parent.tagName === 'DETAILS') parent.open = true; parent = parent.parentElement; }
    }
  });
  document.querySelectorAll('.config-tabs button').forEach(tab => {
    const panel = document.getElementById(tab.getAttribute('aria-controls'));
    const count = panel?.querySelectorAll('.config-field.invalid').length || 0;
    tab.classList.toggle('has-errors', count > 0);
    tab.title = count ? `${count} setting errors in this category` : '';
    tab.setAttribute('aria-label', `${metadata([tab.dataset.tab])[0]}${count ? `, ${count} setting errors` : ''}`);
  });
}

function showMessage(message, kind = '', detail = '') {
  const target = document.getElementById('config-message');
  target.className = `config-message ${kind}`;
  target.textContent = message;
  if (detail) {
    const details = document.createElement('details');
    const summary = document.createElement('summary');
    summary.textContent = 'Details';
    const text = document.createElement('p');
    text.textContent = detail;
    details.append(summary, text);
    target.appendChild(details);
  }
}

let lastReportingStatus = {};
function reportContext(incident) {
  return {incident, status: lastReportingStatus, config: activeConfig || {}};
}
function showReportAction(target, incident) {
  if (target && window.VectorWarpReports)
    window.VectorWarpReports.addContextAction(target, incident ? () => reportContext(incident) : null);
}

function formatHertz(value, unit = 'MHz') {
  return typeof value === 'number' && Number.isFinite(value) ?
    `${value / 1000000} ${unit}` : 'Unavailable';
}

async function refreshUpstreamStatus() {
  const target = document.getElementById('upstream-status');
  if (!target || upstreamPending || document.hidden) return;
  upstreamPending = true;
  try {
    const response = await configFetch(liveApiUrl('/api/upstream/status'),
      {cache: 'no-store'});
    const status = await response.json();
    target.replaceChildren();
    const header = document.createElement('div');
    header.className = 'upstream-header';
    const title = document.createElement('strong');
    title.textContent = 'Receiver status';
    const badge = document.createElement('span');
    const healthy = status.supported && status.available && status.matched;
    badge.className = `upstream-badge ${healthy ? 'good' :
      status.available ? 'warning' : 'bad'}`;
    badge.textContent = status.supported ?
      status.available ? status.issues?.length ? 'Needs attention' : 'Matched' :
        'Unavailable' : 'Not verified';
    header.append(title, badge);
    target.appendChild(header);
    if (!status.supported) {
      const note = document.createElement('p');
      note.textContent = status.message;
      target.appendChild(note);
      return;
    }
    if (!status.available) {
      const note = document.createElement('p');
      note.textContent = `Cannot read Heimdall at ${status.host}:${status.controlPort}. ${status.message || ''}`.trim();
      target.appendChild(note);
      return;
    }
    const grid = document.createElement('div');
    grid.className = 'upstream-grid';
    const note = document.createElement('p');
    note.textContent = 'Receiver-reported settings compared with the saved file, not unsaved edits.';
    target.appendChild(note);
    const rows = [
      ['Frequency', formatHertz(status.expected.centerFrequency),
        formatHertz(status.actual.centerFrequency)],
      ['Sample rate', formatHertz(status.expected.sampleRate, 'MS/s'),
        formatHertz(status.actual.sampleRate, 'MS/s')],
      ['Channels', String(status.expected.channels),
        status.actual.channels === null ? 'Unavailable' : String(status.actual.channels)],
      ['Mode', status.expected.mode, status.actual.mode || 'Unavailable'],
      ['Gain', 'Set in Heimdall', Number.isFinite(status.actual.gain) ?
        status.actual.gain < 0 ? 'Automatic' : `${status.actual.gain.toFixed(1)} dB` :
        'Unavailable']
    ];
    rows.forEach(([labelText, expected, actual]) => {
      const row = document.createElement('div');
      const label = document.createElement('span');
      const values = document.createElement('span');
      const matches = expected === actual || labelText === 'Gain';
      label.textContent = labelText;
      values.textContent = labelText === 'Gain' ? actual :
        matches ? actual : `${actual} · expected ${expected}`;
      row.className = matches ? 'matched' : 'mismatch';
      row.append(label, values);
      grid.appendChild(row);
    });
    target.appendChild(grid);
    if (status.issues?.length) {
      const list = document.createElement('ul');
      status.issues.forEach(issue => {
        const item = document.createElement('li');
        item.textContent = issue.message || (issue.label === 'Upstream state' ||
          issue.label === 'Calibration' ?
          `${issue.label}: ${issue.actual}` :
          `${issue.label} does not match VectorWarp.`);
        list.appendChild(item);
      });
      target.appendChild(list);
    }
  } catch (_) {
    target.innerHTML = '<div class="upstream-header"><strong>Receiver status</strong><span class="upstream-badge bad">Unavailable</span></div><p>Could not check receiver software.</p>';
  } finally { upstreamPending = false; }
}

function setRestartProgress(percent, label, state = '') {
  const progress = document.getElementById('config-restart-progress');
  const bar = document.getElementById('config-restart-bar');
  const copy = document.getElementById('config-restart-label');
  const value = document.getElementById('config-restart-value');
  if (!progress) return;
  progress.hidden = false;
  progress.className = `config-restart-progress ${state}`.trim();
  bar.style.width = `${Math.max(0, Math.min(100, percent))}%`;
  copy.textContent = label;
  value.textContent = state === 'complete' ? 'Done' : state === 'failed' ? 'Error' : 'Working';
  progress.classList.toggle('indeterminate', !state);
}

function wait(milliseconds) {
  return new Promise(resolve => window.setTimeout(resolve, milliseconds));
}

async function readRadarState() {
  const response = await configFetch(
    liveApiUrl('/api/system/status'), {cache: 'no-store'});
  if (!response.ok)
    throw new Error('Cannot read radar status.');
  return response.json();
}

async function monitorRadarRestart(previousState) {
  const started = Date.now();
  // The installed restart oneshot may run for 90 seconds; keep polling long
  // enough to receive its final receipt and a newly produced radar frame.
  const timeout = 130000;
  let upstreamCheckedAt = 0;
  while (Date.now() - started < timeout) {
    let state;
    try {
      state = await readRadarState();
    } catch (_) {
      setRestartProgress(50, 'Reconnecting to VectorWarp…');
    }
    if (state?.restart?.state === 'failed') throw new Error(state.restart.message);
    if (state?.errors?.length) throw new Error(state.errors.join(' '));
    const apiRestarted = state && state.serverId !== previousState.serverId &&
      state.loadedRevision === configRevision;
    const radarReconnected = state && state.restart?.state === 'command-complete' &&
      state.timestampConnections > state.restart.timestampConnections;
    if ((apiRestarted || radarReconnected) && state.radar === 'receiving' && state.lastFrameAt > started) {
      setRestartProgress(100, 'Radar restarted; new frames received.', 'complete');
      return;
    }
    if (apiRestarted && state.radar !== 'receiving' &&
        activeConfig.capture.device.type === 'Kraken' &&
        Date.now() - started > 2000 && Date.now() - upstreamCheckedAt > 2000) {
      upstreamCheckedAt = Date.now();
      let message;
      try {
        const upstream = await configFetch(liveApiUrl('/api/upstream/status'));
        message = upstreamRestartError(await upstream.json());
      } catch (_) { /* A missing optional status feed is not proof of an IQ fault. */ }
      if (message) throw new Error(message);
    }
    if (state) setRestartProgress(50, state.restart?.message || 'Waiting for a new radar connection…');
    await wait(600);
  }
  throw new Error('Settings saved, but restart was not confirmed within 130 seconds. Run vectorwarp status before retrying.');
}

function upstreamRestartError(status) {
  if (!status?.available) return null;
  const calibration = status.issues?.find(issue => issue.label === 'Calibration' &&
    issue.severity === 'error');
  if (calibration)
    return calibration.message || 'Kraken calibration failed. Check the controller log, then restart to retry.';
  const mismatch = status.issues?.find(issue => issue.severity === 'error' &&
    ['capture.fc', 'capture.fs', 'capture.device.channel_count'].includes(issue.field));
  if (!mismatch) return null;
  const format = value => mismatch.field === 'capture.device.channel_count' ? String(value) :
    formatHertz(value, mismatch.field === 'capture.fs' ? 'MS/s' : 'MHz');
  return `${metadata(mismatch.field.split('.'))[0]} mismatch: receiver reports ${format(mismatch.actual)}; settings use ${format(mismatch.expected)}. Match them in Receiver settings and retry restart.`;
}

function receiverSaveMessage(result, restarted) {
  const sync = result?.receiverSync;
  if (sync?.automaticGain?.status === 'verified')
    return `Settings saved. Shared gain ${sync.automaticGain.selectedGainDb} dB passed the headroom check.` +
      (restarted ? ' Radar restarted; new frames received.' : ' Restart is pending.');
  if (sync?.receiverType === 'Kraken' &&
      ['synchronized', 'already-matched'].includes(sync.status)) {
    const confirmed = 'Settings saved. Suite V2 settings confirmed.';
    const lifecycle = restarted ?
      ' Radar restarted; new frames received.' :
      ' Run vectorwarp restart to apply the remaining settings.';
    return `${confirmed}${lifecycle}`;
  }
  if (restarted && sync?.acceptance?.mode === 'replay')
    return 'Settings saved. Replay restarted; new frames received.';
  if (restarted)
    return 'Settings saved. Radar restarted; new frames received.';
  return result?.message ||
    'Settings saved. Run vectorwarp restart to apply them.';
}

function receiverSaveDetails(result) {
  const sync = result?.receiverSync;
  if (sync?.automaticGain?.status === 'verified')
    return 'All receiver channels passed two calibrated-stream headroom checks. Gain is held fixed until the next Apply. Aircraft tracking and bearing accuracy require separate validation.';
  if (sync?.acceptance?.mode === 'replay') return 'No receiver hardware was opened.';
  if (sync?.receiverType === 'Kraken') {
    const check = sync.status === 'synchronized' ?
      'Suite V2 accepted the command and reported matching settings.' :
      sync.status === 'already-matched' ? 'Suite V2 already reported matching settings.' :
      'No Suite V2 command was needed.';
    return `${check} Antenna wiring, serial order and calibration are not independently checked.`;
  }
  if (sync?.receiverType === 'Usrp')
    return 'UHD checks frequency, sample rate, gain, antennas and channel mapping before streaming. This response does not contain tuner readings or an RF test.';
  return 'Receiver settings are sent at startup. Receiving frames is not an independent check of tuner settings or RF performance.';
}

function receiverMayHaveChanged(receipt) {
  return Boolean(receipt && (['partial', 'indeterminate'].includes(receipt.status) ||
    receipt.operations?.some(operation => operation.commandSent && operation.commandOutcome !== 'rejected')));
}

function receiverFailureMessage(result) {
  const message = (result?.errors || [result?.message]).filter(Boolean).join(' ') || 'Unable to save configuration.';
  if (receiverMayHaveChanged(result?.receiverSync))
    return `${message} Receiver settings may already have changed. VectorWarp settings were not saved. Reload and check receiver status before retrying.`;
  return message;
}

function invalidateGeometryAfterReceiverChange() {
  receiverInvalidatedGeometry = true;
  geometryEditor?.invalidate(activeConfig.capture?.device?.array_geometry);
  refreshGeometryEditor();
}

function renderEditor() {
  const target = document.getElementById('config-fields');
  const upstream = document.getElementById('upstream-status');
  if (upstream) upstream.remove();
  target.replaceChildren();
  const tabs = document.createElement('div');
  tabs.className = 'config-tabs';
  tabs.setAttribute('role', 'tablist');
  tabs.setAttribute('aria-label', 'Settings categories');
  const panels = document.createElement('div');
  panels.className = 'config-tab-panels';
  const tabOrder = ['capture', 'location', 'process', 'truth', 'save', 'network'];
  const entries = Object.entries(activeConfig).filter(([key]) =>
    settingsMode === 'advanced' || key !== 'network').sort(([left], [right]) =>
    (tabOrder.includes(left) ? tabOrder.indexOf(left) : 100) -
    (tabOrder.includes(right) ? tabOrder.indexOf(right) : 100));
  let savedTab = null;
  try { savedTab = sessionStorage.getItem('blah2-settings-tab'); }
  catch (_) { /* Remembering a tab is optional. */ }
  const initialTab = capabilities.setupRequired ? 'capture' : entries.some(([key]) => key === savedTab) ?
    savedTab : entries[0]?.[0];
  const activate = selectedKey => {
    tabs.querySelectorAll('[role="tab"]').forEach(button => {
      const selected = button.dataset.tab === selectedKey;
      button.classList.toggle('active', selected);
      button.setAttribute('aria-selected', String(selected));
      button.tabIndex = selected ? 0 : -1;
    });
    panels.querySelectorAll('[role="tabpanel"]').forEach(panel => {
      panel.hidden = panel.dataset.tab !== selectedKey;
    });
    try { sessionStorage.setItem('blah2-settings-tab', selectedKey); }
    catch (_) { /* Remembering a tab is optional. */ }
  };
  activateSettingsTab = selectedKey => {
    if (!tabs.querySelector(`[data-tab="${selectedKey}"]`)) return;
    activate(selectedKey);
  };
  entries.forEach(([key, value]) => {
    const [title, description] = metadata([key]);
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.tab = key;
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-controls', `settings-panel-${key}`);
    const strong = document.createElement('strong');
    const small = document.createElement('small');
    strong.textContent = title;
    small.textContent = description;
    button.append(strong, small);
    button.addEventListener('click', () => activate(key));
    button.addEventListener('keydown', event => {
      if (!['ArrowLeft', 'ArrowRight'].includes(event.key)) return;
      const buttons = [...tabs.querySelectorAll('[role="tab"]')];
      const offset = event.key === 'ArrowRight' ? 1 : -1;
      const next = buttons[(buttons.indexOf(button) + offset + buttons.length) %
        buttons.length];
      next.focus();
      activate(next.dataset.tab);
    });
    tabs.appendChild(button);
    const panel = document.createElement('section');
    panel.id = `settings-panel-${key}`;
    panel.dataset.tab = key;
    panel.setAttribute('role', 'tabpanel');
    panel.setAttribute('aria-label', title);
    panel.className = 'config-tab-panel';
    const receiverType = activeConfig.capture?.device?.type;
    if (key === 'location') panel.appendChild(renderSiteSettings());
    else if (value !== null && typeof value === 'object' && !Array.isArray(value))
      panel.appendChild(group(key, value, [], true, {skipPath: path =>
        (key === 'process' && receiverSpecificProcessPath(path)) ||
        (key === 'capture' && path.startsWith('capture.aircraft_clips')) ||
        simpleSettingHidden(path)}));
    else
      panel.appendChild(field(value, [key]));
    if (key === 'capture') {
      if (settingsMode === 'advanced') panel.appendChild(renderReceiverFeatures());
      else {
        const note = document.createElement('p');
        note.className = 'config-simple-note';
        note.textContent = 'Advanced mode contains receiver-specific array, synchronization, and diagnostic controls.';
        panel.appendChild(note);
      }
      if (['RspDuo', 'Usrp', 'HackRF'].includes(activeConfig.capture?.device?.type)) {
        const startup = document.createElement('section');
        startup.id = 'receiver-startup';
        startup.className = 'upstream-status receiver-startup';
        panel.appendChild(startup);
        renderReceiverStartup(null, startup);
      }
      if (activeConfig.capture?.device?.type === 'RspDuo') {
        const health = document.createElement('section');
        health.id = 'receiver-health';
        health.className = 'upstream-status receiver-health';
        health.textContent = 'RSPduo overload status unavailable.';
        panel.appendChild(health);
        renderReceiverOverload(null, health);
      }
      panel.appendChild(renderReceiverSetup());
      if (upstream) panel.appendChild(upstream);
    }
    if (key === 'save') {
      const note = document.createElement('p');
      note.className = 'config-capability-note';
      if (activeConfig.capture?.aircraft_clips &&
          typeof activeConfig.capture.aircraft_clips === 'object') {
        panel.appendChild(group('aircraft_clips', activeConfig.capture.aircraft_clips,
          ['capture']));
        note.textContent = 'Start and stop manual raw-IQ recordings from Live radar. Automatic clips above run only when enabled and an eligible ADS-B aircraft meets the selected bounds.';
      } else {
        note.textContent = 'Manual raw-IQ recording is available from Live radar. Automatic aircraft clips require a backend version that advertises those settings.';
      }
      panel.appendChild(note);
    }
    panels.appendChild(panel);
  });
  if (settingsMode === 'simple') {
    const heading = document.createElement('div');
    heading.className = 'config-section-heading';
    heading.innerHTML = '<h2>Essential settings</h2><p>Only the settings normally needed to get a receiver running are shown below.</p>';
    target.append(heading);
  }
  target.append(tabs, panels);
  activate(initialTab);
  displayFieldErrors(validationErrors);
  displayValidationNotices();
  updateDirtyState();
  refreshSetupAssistant();
}

function renderReceiverOverload(status, target = typeof document === 'undefined' ? null : document.getElementById('receiver-health')) {
  if (typeof renderReceiverHealth === 'function')
    renderReceiverHealth(status, target, true);
}

function receiverStartupShell(target, title, text) {
  let heading = target.querySelector('[data-receiver-startup-heading]');
  if (!heading) {
    heading = document.createElement('h3');
    heading.dataset.receiverStartupHeading = '';
    target.appendChild(heading);
  }
  heading.textContent = title;
  let message = target.querySelector('[data-receiver-startup-message]');
  if (!message) {
    message = document.createElement('p');
    message.dataset.receiverStartupMessage = '';
    message.setAttribute('role', 'status');
    target.appendChild(message);
  }
  message.textContent = text;
}

function receiverStartupMismatch(target, visible) {
  let mismatch = target.querySelector('[data-receiver-startup-mismatch]');
  if (!visible) { mismatch?.remove(); return; }
  if (!mismatch) {
    mismatch = document.createElement('p');
    mismatch.dataset.receiverStartupMismatch = '';
    const details = target.querySelector('[data-receiver-startup-details]');
    target.insertBefore(mismatch, details || null);
  }
  mismatch.textContent = 'Running settings differ. Use Save & Restart to apply these values.';
}

function receiverStartupDetails(target, {summaryText, captionText, rows, noteText}) {
  let details = target.querySelector('[data-receiver-startup-details]');
  if (!details) {
    details = document.createElement('details');
    details.dataset.receiverStartupDetails = '';
    const summary = document.createElement('summary');
    summary.dataset.receiverStartupSummary = '';
    const table = document.createElement('table');
    table.dataset.receiverStartupTable = '';
    const caption = document.createElement('caption');
    caption.dataset.receiverStartupCaption = '';
    const body = document.createElement('tbody');
    body.dataset.receiverStartupRows = '';
    table.append(caption, body);
    const note = document.createElement('p');
    note.dataset.receiverStartupNote = '';
    details.append(summary, table, note);
    target.appendChild(details);
  }
  details.querySelector('[data-receiver-startup-summary]').textContent = summaryText;
  details.querySelector('[data-receiver-startup-caption]').textContent = captionText;
  details.querySelector('[data-receiver-startup-note]').textContent = noteText;
  const body = details.querySelector('[data-receiver-startup-rows]');
  while (body.rows.length > rows.length) body.deleteRow(-1);
  rows.forEach(([name, value], index) => {
    let row = body.rows[index];
    if (!row) {
      row = body.insertRow();
      const label = document.createElement('th');
      label.scope = 'row';
      const cell = document.createElement('td');
      row.append(label, cell);
    }
    row.cells[0].textContent = name;
    row.cells[1].textContent = String(value);
  });
}

function clearReceiverStartupDetails(target) {
  target.querySelector('[data-receiver-startup-mismatch]')?.remove();
  target.querySelector('[data-receiver-startup-details]')?.remove();
}

function renderReceiverStartup(status, target = typeof document === 'undefined' ? null : document.getElementById('receiver-startup')) {
  if (!target) return;
  if (activeConfig.capture?.device?.type === 'HackRF') {
    renderHackrfStartup(status, target);
    return;
  }
  if (activeConfig.capture?.device?.type === 'Usrp') {
    renderUsrpStartup(status, target);
    return;
  }
  const processor = status?.processorFresh && status.processor?.receiver === 'RspDuo' &&
    status.processor?.input === 'live' ? status.processor : null;
  const receipt = processor?.receiverStartup;
  const message = !processor ? 'No current RSPduo status. Saved settings apply at startup.' :
    processor.state === 'error' ? `Receiver error: ${processor.error}` :
    !receipt ? 'Startup settings unavailable. Rebuild SDRplay support to see them.' :
    receipt.status !== 'accepted' ? 'Applying settings through SDRplay…' :
    processor.state === 'stopped' ? 'Receiver stopped. SDRplay accepted the last startup settings.' : status.radar === 'receiving' ?
    'SDRplay accepted settings; receiving radar frames.' :
    'SDRplay accepted settings; waiting for radar frames.';
  receiverStartupShell(target, 'SDRplay startup', message);
  if (!receipt) { clearReceiverStartupDetails(target); return; }
  const requested = receipt.requested;
  const device = activeConfig.capture.device;
  const matches = requested.frequency === activeConfig.capture.fc &&
    requested.sampleRate === activeConfig.capture.fs && requested.serial === (device.serial || '') &&
    ['agcSetPoint', 'bandwidthNumber', 'gainReduction', 'lnaState', 'dabNotch', 'rfNotch']
      .every(key => JSON.stringify(requested[key]) === JSON.stringify(device[key]));
  receiverStartupMismatch(target, !matches);
  const rows = [
    ['Selected receiver', receipt.selected?.serial || 'Not selected'],
    ['SDRplay API', receipt.sdk.version === null ? 'Not checked' : Number(receipt.sdk.version).toFixed(2)],
    ['Frequency', `${requested.frequency / 1000000} MHz`],
    ['Output sample rate', `${requested.sampleRate} S/s`],
    ['AGC', requested.bandwidthNumber ? `${requested.bandwidthNumber} Hz; ${requested.agcSetPoint} dBFS target` : 'Off'],
    ['Gain reduction A / B', `${requested.gainReduction.join(' / ')} dB`],
    ['LNA state', requested.lnaState],
    ['DAB / RF notch', `${requested.dabNotch ? 'On' : 'Off'} / ${requested.rfNotch ? 'On' : 'Off'}`],
    ['IF / bandwidth', `${requested.ifFrequencyKhz} / ${requested.ifBandwidthKhz} kHz`],
    ['Decimation', `${requested.decimation}×`],
    ['Sample counter policy / effective ratio',
      `${requested.sampleCounterScalePolicy || requested.sampleCounterScale} / ${requested.sampleCounterScale}`],
    ['SDK stages completed', Object.entries(receipt.sdk.stages).filter(([, ok]) => ok).map(([stage]) => stage).join(', ') || 'None']
  ];
  receiverStartupDetails(target, {summaryText: 'Startup details',
    captionText: 'Values sent to SDRplay, not measured tuner values', rows,
    noteText: 'These checks do not measure RF performance, filtering or phase coherence.'});
}

function renderHackrfStartup(status, target = typeof document === 'undefined' ? null : document.getElementById('receiver-startup')) {
  if (!target) return;
  const processor = status?.processorFresh && status.processor?.receiver === 'HackRF' &&
    status.processor?.input === 'live' ? status.processor : null;
  const receipt = processor?.receiverStartup;
  const message = !processor ? 'No current HackRF status. Saved settings apply at startup.' :
    processor.state === 'error' ? `Receiver error: ${processor.error}` :
    !receipt ? 'HackRF startup receipt unavailable. Rebuild HackRF support to see it.' :
    receipt.status !== 'accepted' ? 'Applying settings through libhackrf…' :
    !receipt.setterAcceptance ? 'HackRF started without qualified setter acceptance.' :
    processor.state === 'stopped' ? 'Receiver stopped. The last HackRF startup calls were accepted.' : status.radar === 'receiving' ?
    'Both HackRF startup paths accepted settings; receiving provisional paired frames.' :
    'Both HackRF startup paths accepted settings; waiting for provisional paired frames.';
  receiverStartupShell(target, 'Dual HackRF startup', message);
  if (!receipt) { clearReceiverStartupDetails(target); return; }
  const requested = receipt.requested;
  const device = activeConfig.capture.device;
  const matches = requested.frequency === activeConfig.capture.fc &&
    requested.sampleRate === activeConfig.capture.fs &&
    JSON.stringify(requested.serials) === JSON.stringify(device.serial) &&
    JSON.stringify(requested.lnaGainDb) === JSON.stringify(device.gain_lna) &&
    JSON.stringify(requested.vgaGainDb) === JSON.stringify(device.gain_vga) &&
    JSON.stringify(requested.ampEnabled) === JSON.stringify(device.amp_enable);
  receiverStartupMismatch(target, !matches);
  const resolved = receipt.deviceIdentity?.resolvedSerials || [];
  const yesNo = values => Array.isArray(values) ?
    values.map(value => value ? 'On' : 'Off').join(' / ') : 'Unavailable';
  const rows = [
    ['Resolved reference / surveillance', resolved.join(' / ') || 'Unavailable'],
    ['Frequency', `${requested.frequency} Hz`],
    ['Sample rate', `${requested.sampleRate} S/s`],
    ['LNA gain reference / surveillance', `${requested.lnaGainDb.join(' / ')} dB`],
    ['VGA gain reference / surveillance', `${requested.vgaGainDb.join(' / ')} dB`],
    ['RF amplifier reference / surveillance', yesNo(requested.ampEnabled)],
    ['Clock output / trigger input role', `${receipt.synchronization?.clockOutputRole || 'Unavailable'} / ${receipt.synchronization?.hardwareTriggerRole || 'Unavailable'}`],
    ['Baseband filter', 'libhackrf automatic (at most 0.75 × sample rate)']
  ];
  receiverStartupDetails(target, {summaryText: 'Startup setter details',
    captionText: 'libhackrf setter acceptance, not RF or synchronization readback', rows,
    noteText: 'The API does not read back RF settings, shared-clock/trigger wiring, or hardware time. Pairing and event time remain provisional.'});
}

function renderUsrpStartup(status, target = typeof document === 'undefined' ? null : document.getElementById('receiver-startup')) {
  if (!target) return;
  const processor = status?.processorFresh && status.processor?.receiver === 'Usrp' &&
    status.processor?.input === 'live' ? status.processor : null;
  const receipt = processor?.receiverStartup;
  const message = !processor ? 'No current USRP status. Saved settings apply at startup.' :
    processor.state === 'error' ? `Receiver error: ${processor.error} Reconnect the USRP if it was unplugged, then run vectorwarp restart.` :
    !receipt ? 'USRP startup readback unavailable. Rebuild UHD support to see it.' :
    receipt.status !== 'accepted' ? 'Applying settings through UHD…' :
    !receipt.readbackAvailable ? 'USRP started without qualified UHD setting readback.' :
    processor.state === 'stopped' ? 'Receiver stopped. UHD read back the last startup settings.' : status.radar === 'receiving' ?
    'UHD read back both channels; receiving radar frames.' :
    'UHD read back both channels; waiting for radar frames.';
  receiverStartupShell(target, 'USRP startup', message);
  if (!receipt) { clearReceiverStartupDetails(target); return; }
  const requested = receipt.requested;
  const device = activeConfig.capture.device;
  const matches = requested.frequency === activeConfig.capture.fc &&
    requested.sampleRate === activeConfig.capture.fs && requested.address === device.address &&
    requested.subdevices === device.subdev &&
    JSON.stringify(requested.antennas) === JSON.stringify(device.antenna) &&
    JSON.stringify(requested.gains) === JSON.stringify(device.gain);
  receiverStartupMismatch(target, !matches);
  const applied = receipt.appliedReadback;
  const coherence = receipt.coherence;
  const identity = coherence?.deviceIdentity;
  const joined = (values, suffix = '') => Array.isArray(values) ?
    values.map(value => `${value}${suffix}`).join(' / ') : 'Unavailable';
  const rows = [
    ['Device address', requested.address],
    ['Motherboard', coherence ? `${coherence.motherboard}${identity?.motherboardSerial ?
      `; serial ${identity.motherboardSerial}` : '; serial unavailable'}` : 'Not attested'],
    ['RX identity A / B', joined(identity?.rxIds)],
    ['Clock / time source', coherence ? `${coherence.clockSource} / ${coherence.timeSource}` : 'Not attested'],
    ['Subdevices', applied?.subdevices || requested.subdevices],
    ['Frequency A / B', joined(applied?.perChannelFrequencyHz, ' Hz')],
    ['Sample rate A / B', joined(applied?.perChannelSampleRateHz, ' S/s')],
    ['Gain A / B', joined(applied?.perChannelGainDb, ' dB')],
    ['Antenna A / B', joined(applied?.perChannelAntenna)]
  ];
  receiverStartupDetails(target, {summaryText: 'Startup readback details',
    captionText: 'UHD device-API getter values, not a physical RF measurement', rows,
    noteText: 'These getters do not measure RF performance or map UHD device time to UTC. Exact event-time scoring remains unavailable.'});
}

function renderReceiverSetup() {
  if (window.blah2ReceiverDiscoveryTimer)
    window.clearTimeout(window.blah2ReceiverDiscoveryTimer);
  const section = document.createElement('section');
  section.className = 'upstream-status';
  section.id = 'receiver-setup';
  const heading = document.createElement('h3');
  heading.textContent = 'Receiver software';
  const description = document.createElement('p');
  description.textContent = 'Find receivers and check their software without changing settings.';
  const check = document.createElement('button');
  check.type = 'button'; check.className = 'button-secondary';
  check.textContent = 'Check receiver software';
  const output = document.createElement('div');
  output.setAttribute('role', 'status'); output.setAttribute('aria-live', 'polite');
  let buildPolls = 0;
  let buildPollTimer = null;
  const paragraph = text => { const p = document.createElement('p'); p.textContent = text; output.appendChild(p); return p; };
  const post = async (route, body, timeout = 30000, intent = 'receiver-management-v1') => {
    const response = await configFetch(liveApiUrl(route), {method: 'POST', headers: {
      'Content-Type': 'application/json', 'X-VectorWarp-Intent': intent}, body: JSON.stringify(body)}, timeout);
    const result = await response.json();
    if (!response.ok) throw new Error(result.errors?.join(' ') || result.message || 'Receiver software check failed.');
    return result;
  };
  const button = (label, callback) => {
    const element = document.createElement('button');
    element.type = 'button'; element.className = 'button-secondary'; element.textContent = label;
    element.addEventListener('click', async () => {
      if (saveInProgress || restartInProgress) return;
      element.disabled = true;
      try { await callback(element); } catch (error) { paragraph(error.message); }
      finally { if (element.isConnected) element.disabled = false; }
    });
    output.appendChild(element);
    return element;
  };
  const sdrplayLink = () => {
    const link = document.createElement('a');
    link.href = 'https://sdrplay.com/hardware-api/';
    link.textContent = 'Download SDRplay API';
    link.target = '_blank'; link.rel = 'noopener noreferrer';
    output.appendChild(link);
  };
  const refreshBuildStatus = () => {
    if (buildPollTimer) window.clearTimeout(buildPollTimer);
    if (buildPolls >= 40 || !section.isConnected) return;
    buildPollTimer = window.setTimeout(() => {
      buildPollTimer = null;
      buildPolls++;
      discover();
    }, 5000);
  };
  const settingApplication = (receiver, item) => {
    const label = metadata(item.configField.split('.'), receiver.type)[0];
    if (item.direction === 'browser-to-upstream-after-ack-and-readback')
      return `${label}: sent to Suite V2, then checked against its reply and updated status.`;
    if (item.direction === 'upstream-authoritative-mismatch-block')
      return `${label}: must match Suite V2 before applying. VectorWarp does not change it in Suite V2.`;
    if (item.direction === 'config-only')
      return `${label}: used by VectorWarp only; not sent to Suite V2.`;
    if (receiver.type === 'Usrp')
      return `${label}: applied through UHD after Save & Restart. Both channels are checked before streaming.`;
    if (receiver.type === 'RspDuo')
      return `${label}: applied through SDRplay API v3 at startup. SDK errors are reported; tuner values are not read back.`;
    if (receiver.type === 'HackRF')
      return `${label}: applied to both HackRFs at startup. Driver errors are checked; tuner values are not read back.`;
    return `${label}: saved for this receiver.`;
  };
  async function reviewAction(receiverType, actionId) {
    if (serializeConfig(activeConfig) !== originalConfig)
      throw new Error('Save or discard your edits first. Receiver actions use saved settings.');
    const plan = await post('/api/receivers/plan', {receiverType, actionId});
    if (plan.status === 'not-required') { paragraph(plan.message); return; }
    if (plan.requiresAuthorization !== false) {
      paragraph('Run this command on the VectorWarp computer to authorize this action, then return here.');
      const command = document.createElement('pre'); command.textContent = plan.authorizationCommand;
      output.appendChild(command);
    }
    paragraph(`Review: ${plan.review}. The plan expires in ${plan.lifetimeSeconds} seconds.`);
    for (const item of plan.transaction?.changes || [])
      paragraph(`Install ${item.name} ${item.version} (${item.architecture}; ${item.origin} ${item.archive}; ${item.site}).`);
    button(plan.requiresAuthorization === false ? 'Run reviewed action' : 'Run authorized action', async element => {
      if (serializeConfig(activeConfig) !== originalConfig || configRevision !== plan.configRevision)
        throw new Error('Settings changed. Check receiver software and review the action again.');
      element.textContent = 'Running…';
      let result;
      try { result = await post('/api/receivers/execute', {nonce: plan.nonce, configRevision: plan.configRevision}, 210000); }
      catch (error) {
        element.remove();
        throw new Error(`${error.message} Check receiver software again before retrying.`);
      }
      element.remove();
      paragraph(result.message);
      await discover();
    });
  }
  async function discover() {
    if (buildPollTimer) { window.clearTimeout(buildPollTimer); buildPollTimer = null; }
    check.disabled = true;
    output.replaceChildren();
    paragraph('Checking receiver software…');
    try {
      const result = await post('/api/receivers/discover', {});
      output.replaceChildren();
      const detected = (result.receivers || []).filter(receiver =>
        receiver.capabilities?.detected === true);
      const detectedTypes = new Set(detected.map(receiver => receiver.type));
      const newlyDetected = detected.filter(receiver => !lastDetectedReceiverTypes.has(receiver.type));
      if (capabilities.setupRequired && activeConfig.capture?.device?.type === 'Unconfigured') {
        if (detected.length === 1) {
          const receiver = detected[0];
          paragraph(`${receiver.label} detected. Confirm it before VectorWarp saves or starts processing.`);
          if (receiver.capabilities.liveCompiled ||
              (receiver.type === 'RspDuo' && receiver.capabilities.localBuildable))
            button(`Use detected ${receiver.label}`, () => switchDevice(receiver.type));
        } else if (detected.length > 1) {
          paragraph(`${detected.length} receivers detected. Choose the one to configure below.`);
        } else {
          paragraph('No supported local receiver was detected. Connect one and wait for another check, or choose a receiver manually for remote/offline setup.');
        }
      } else {
        for (const receiver of newlyDetected.filter(item => item.type !== activeConfig.capture?.device?.type)) {
          paragraph(`New ${receiver.label} detected. VectorWarp will not interrupt the active receiver.`);
          if (receiver.capabilities.liveCompiled ||
              (receiver.type === 'RspDuo' && receiver.capabilities.localBuildable))
            button(`Configure ${receiver.label}`, () => switchDevice(receiver.type));
        }
      }
      lastDetectedReceiverTypes = detectedTypes;
      if (result.configRevision !== configRevision) paragraph('Saved settings changed. Reload them before continuing.');
      if (!result.buildCapabilitiesKnown) paragraph('This build does not list its supported receivers. Live capture support is unknown.');
      for (const receiver of result.receivers || []) {
        const compiled = receiver.capabilities.liveCompiled ? 'adapter built' : 'adapter not built';
        const runtime = receiver.capabilities.runtimeLoadable === true ? 'software loads' :
          receiver.capabilities.runtimeLoadable === false ? 'software cannot load' : 'software not checked';
        const detection = receiver.detection.configuredIdentityMatched === false ? 'selected device not found' : receiver.detection.state;
        const selected = receiver.type === activeConfig.capture?.device?.type;
        const card = document.createElement('article');
        card.className = `receiver-software-card${selected ? ' selected' : ''}`;
        card.dataset.receiverType = receiver.type;
        const cardHeader = document.createElement('header');
        const cardTitle = document.createElement('strong');
        cardTitle.textContent = receiver.label;
        const cardState = document.createElement('span');
        cardState.className = receiver.capabilities.liveCompiled && receiver.capabilities.runtimeLoadable !== false ?
          'upstream-badge good' : 'upstream-badge warning';
        cardState.textContent = selected ? 'Selected' : receiver.capabilities.detected ? 'Detected' : 'Available option';
        cardHeader.append(cardTitle, cardState);
        const facts = document.createElement('dl');
        const fact = (label, value) => {
          const row = document.createElement('div');
          const term = document.createElement('dt'); term.textContent = label;
          const detail = document.createElement('dd'); detail.textContent = value;
          row.append(term, detail); facts.appendChild(row);
        };
        fact('VectorWarp adapter', compiled);
        fact('Driver / API', runtime);
        fact('Connected device', detection);
        fact('Dependencies', receiver.dependencies.state);
        if (receiver.managedService.required) fact('Receiver service', receiver.managedService.state);
        fact('Saved connection', receiver.upstream.availability === 'available' ? 'available' :
          receiver.upstream.availability || 'not configured');
        card.append(cardHeader, facts);
        output.appendChild(card);
        if (receiver.upstream.availability === 'available') paragraph(`${receiver.label}: saved connection is available and will be reused. Settings are checked when saving.`);
        if (receiver.type === 'RspDuo' && receiver.managedService.state === 'running')
          paragraph('SDRplay API is running; no service restart needed.');
        if (receiver.type === 'RspDuo' && receiver.dependencies.state !== 'installed') {
          paragraph(receiver.dependencies.state === 'missing' ?
            'SDRplay API was not found. Download it from SDRplay and accept its license locally.' :
            'Could not check SDRplay API. Use the link below if you need to install it.');
          sdrplayLink();
        }
        else if (receiver.type === 'RspDuo' && receiver.managedService.state === 'stopped')
          paragraph('SDRplay API is stopped. Save & Restart starts the standard local service. Custom services need administrator setup.');
        else if (receiver.type === 'RspDuo' && receiver.managedService.state === 'unknown')
          paragraph('SDRplay API service status could not be checked.');
        if (receiver.type === 'RspDuo') {
          try {
            const response = await configFetch(liveApiUrl('/api/sdrplay-build'), {}, 5000);
            const build = await response.json();
            const relevantProgress = build.progress && (!build.progress.kit_id ||
              build.progress.kit_id === build.kit_id) ? build.progress : null;
            if (build.buildable && ['missing', 'stale'].includes(build.state)) {
              paragraph(`SDRplay adapter: ${relevantProgress?.state || build.state}. ${relevantProgress?.reason || build.reason || 'Building does not start radar.'}`);
              sdrplayLink();
              button('Build SDRplay support', async element => {
                element.textContent = 'Starting build…';
                const result = await post('/api/sdrplay-build', {}, 210000, 'sdrplay-local-build-v1');
                paragraph(result.message || 'Build requested. Checking progress…');
                buildPolls = 0;
                refreshBuildStatus();
              });
            } else if (build.buildable && build.state === 'current') paragraph('SDRplay adapter is up to date. See device and service status above.');
            else if (build.buildable) { paragraph(build.reason || 'Cannot check the SDRplay adapter. No build started.'); sdrplayLink(); }
            else if (build.reason) paragraph(build.reason);
            if (relevantProgress && ['queued', 'running'].includes(relevantProgress.state)) refreshBuildStatus();
          } catch (_) { paragraph('Cannot check the SDRplay adapter. No build started.'); }
        }
        if (receiver.setupGuide?.length) button(`Setup guide: ${receiver.label}`, () => {
          for (const step of receiver.setupGuide) {
            paragraph(step.text);
            if (step.command) {
              const command = document.createElement('pre'); command.textContent = step.command;
              output.appendChild(command);
            }
            if (step.link) {
              const url = new URL(step.link);
              if (url.protocol !== 'https:') continue;
              const link = document.createElement('a'); link.href = url.href;
              link.textContent = step.label; link.target = '_blank'; link.rel = 'noopener noreferrer';
              output.appendChild(link);
            }
          }
        });
        if (receiver.settings?.length) button(`How settings apply: ${receiver.label}`, () => {
          paragraph(`${receiver.label}: software checks do not test antenna wiring, calibration or RF performance.`);
          for (const item of receiver.settings) paragraph(settingApplication(receiver, item));
        });
        if (receiver.capabilities.liveCompiled ||
            (receiver.type === 'RspDuo' && receiver.capabilities.localBuildable)) button(`Choose ${receiver.label} for settings`, () => {
          switchDevice(receiver.type);
          paragraph(receiver.capabilities.liveCompiled ?
            `${receiver.label} selected. Save to apply.` :
            `${receiver.label} selected but not saved. Build its adapter before live capture.`);
        });
        for (const action of result.management?.actions?.filter(item => item.receiverType === receiver.type) || []) {
          if (action.available && !action.ready) button(action.kind === 'start-service' ?
            (receiver.type === 'RspDuo' ? 'Review Start SDRplay' : 'Review receiver service start') : 'Review missing dependency install',
            () => reviewAction(receiver.type, action.id));
          else if (!action.available) paragraph(action.message || 'The installed action needs local review.');
        }
      }
      if (!result.managementAvailable) paragraph(result.management?.message || 'Local receiver management is unavailable.');
      else if (!result.management.actions?.length) paragraph('Automatic software setup is unavailable in this build. Installed software and remote receivers can still be used.');
      if (result.processor?.state === 'error') paragraph(`Processor input error: ${result.processor.error}`);
      else paragraph('Use Save & Restart to test reception. Software checks do not test antenna wiring or calibration.');
      for (const error of result.errors || []) paragraph(error.message);
    } catch (error) { output.replaceChildren(); paragraph(error.message); }
    finally { check.disabled = false; }
  }
  check.addEventListener('click', discover);
  section.append(heading, description, check, output);
  return section;
}

async function saveConfiguration(mode) {
  const saveLater = mode === 'pending';
  const button = document.getElementById(saveLater ? 'config-save-later' : 'config-save');
  if (saveInProgress || restartInProgress || pendingLiveConfig || document.getElementById('site-dialog')) return;
  saveInProgress = true;
  updateDirtyState();
  button.textContent = 'Saving…';
  if (validationTimer) window.clearTimeout(validationTimer);
  let saved = false;
  let writeAttempted = false;
  let responseReceived = false;
  let fileOutcomeUnknown = false;
  try {
    if (!(await validateActiveConfiguration())) return;
    const snapshot = serializeConfig(activeConfig);
    const body = JSON.stringify(activeConfig);
    let previousState = {};
    try { previousState = await readRadarState(); } catch (_) { /* Already offline. */ }
    if (snapshot !== serializeConfig(activeConfig) || snapshot !== validatedSnapshot) {
      showMessage('A setting changed during validation. Review it and save again.', 'warning');
      return;
    }
    restartInProgress = true;
    const measuredGain = !saveLater && activeConfig.capture?.replay?.state !== true &&
      activeConfig.capture?.device?.type === 'Kraken' &&
      activeConfig.capture?.device?.heimdall?.gain_control === 'measured';
    setRestartProgress(30, saveLater ? 'Saving without applying…' : measuredGain ?
      'Measuring receiver headroom and calibrating shared gain…' : 'Checking receiver settings…');
    document.querySelectorAll('#config-fields input, #config-fields select').forEach(input => { input.disabled = true; });
    writeAttempted = true;
    const response = await configFetch(liveApiUrl(saveLater ? '/api/config?mode=pending&restart=false' :
      `/api/config?restart=${capabilities.restartAvailable ? 'true' : 'false'}`), {
      method: 'PUT',
      headers: {'Content-Type': 'application/json', 'If-Match': `"${configRevision}"`,
        'X-VectorWarp-Intent': 'config-write-v1',
        'X-VectorWarp-Receiver-Sync': saveLater ? 'save-pending-v1' : 'synchronize-v1'},
      body
    // Receiver synchronization has a 75s absolute deadline across all commands
    // and idle waits. Allow another 15s for bind checks and response transport.
    // Transport loss still leaves the outcome unknown; never retry automatically.
    }, measuredGain ? MEASURED_GAIN_SAVE_TIMEOUT_MS : CONFIG_SAVE_TIMEOUT_MS);
    const result = await response.json();
    responseReceived = true;
    if (!response.ok) {
      fileOutcomeUnknown = saveLater && result.code === 'PENDING_SAVE_PERSISTENCE_FAILED';
      if (receiverMayHaveChanged(result.receiverSync)) invalidateGeometryAfterReceiverChange();
      throw new Error(receiverFailureMessage(result));
    }
    saved = true;
    receiverApplyPending = result.receiverSync?.status === 'saved-pending';
    rememberSites();
    configRevision = result.revision;
    if (result.config) activeConfig = result.config;
    receiverInvalidatedGeometry = false;
    originalConfig = serializeConfig(activeConfig);
    capabilities.setupRequired = false;
    pendingLiveConfig = null;
    restartRetry = false;
    if (result.receiverSync?.status === 'synchronized' ||
        result.receiverSync?.status === 'already-matched')
      setRestartProgress(45,
        'Suite V2 settings confirmed…');
    if (saveLater) {
      setRestartProgress(100, 'Saved for later; not applied.', 'complete');
      showMessage(result.message, 'warning');
    } else if (result.restarting) {
      rememberApiPort(activeConfig);
      if (window.BLAH2_API_PORT) window.BLAH2_API_PORT = String(activeConfig.network.ports.api);
      // An explicit old query port must not pin restart polling to a dead port.
      const url = new URL(window.location.href);
      if (url.searchParams.has('apiPort')) {
        url.searchParams.set('apiPort', activeConfig.network.ports.api);
        window.history.replaceState(null, '', url);
      }
      await monitorRadarRestart(previousState);
      showMessage(receiverSaveMessage(result, true), 'success', receiverSaveDetails(result));
    } else {
      setRestartProgress(100, 'Saved; manual restart required.', 'complete');
      showMessage(receiverSaveMessage(result, false), 'warning', receiverSaveDetails(result));
    }
  } catch (error) {
    const unknownOutcome = writeAttempted && !responseReceived;
    if (writeAttempted && !saveLater) receiverApplyPending = true;
    if (unknownOutcome && !saveLater) invalidateGeometryAfterReceiverChange();
    showMessage(unknownOutcome ? saveLater ?
      `${error.message || 'Connection lost.'} Save not confirmed. No receiver changes or restart requested. Reload saved settings before retrying.` :
      `${error.message || 'Connection lost.'} Save not confirmed; receiver settings may still change. Reload and check receiver status before retrying.` :
      error.message || 'Unable to save configuration.', 'error');
    const failureKind = saved ? 'restart' : fileOutcomeUnknown || unknownOutcome ?
      'config-persistence' : 'receiver-sync';
    const reportIncident = window.VectorWarpReports?.classifyFailure(failureKind, {
      message: error.message || 'Unable to save configuration.',
      details: {writeAttempted, responseReceived, saved, fileOutcomeUnknown, unknownOutcome}});
    showReportAction(document.getElementById('config-message'), reportIncident);
    setRestartProgress(100, saved ? 'Saved; restart needs attention.' : unknownOutcome || fileOutcomeUnknown ?
      'Save not confirmed.' : 'Not saved.', 'failed');
    restartRetry = saved && !saveLater && capabilities.restartAvailable;
  } finally {
    saveInProgress = false;
    restartInProgress = false;
    refreshGeometryEditor();
    document.querySelectorAll('#config-fields input, #config-fields select').forEach(input => {
      input.disabled = input.dataset.originalDisabled === 'true';
    });
    updateDirtyState();
    if (saveLater) button.textContent = 'Save for later';
  }
}

async function syncConfigurationFromServer() {
  if (!activeConfig || saveInProgress || restartInProgress || configSyncInProgress || document.hidden || document.getElementById('site-dialog')) return;
  configSyncInProgress = true;
  try {
    const response = await configFetch(liveApiUrl('/api/config'), {cache: 'no-store'});
    if (!response.ok) return;
    const liveConfig = await response.json();
    const serialized = serializeConfig(liveConfig);
    const revision = response.headers.get('ETag')?.replace(/^"|"$/g, '');
    if (serialized === originalConfig) { configRevision = revision; return; }
    // Never replace the editor from a background poll. Even a clean form can
    // have a focused field, a native picker, or an expanded receiver section.
    // Keep the confirmed remote revision pending and let the explicit Reload
    // action apply it through the existing reset handler.
    pendingLiveConfig = liveConfig;
    pendingRevision = revision;
    updateDirtyState();
    showMessage('Settings changed elsewhere. Reload saved settings when ready.', 'warning');
  } catch (_) {
    /* The header reports connectivity; keep the last verified values visible. */
  } finally {
    configSyncInProgress = false;
  }
}

async function renderConfiguration() {
  const target = document.getElementById('configuration');
  if (!target) return;
  try {
    const [configResponse, capabilitiesResponse] = await Promise.all([
      configFetch(liveApiUrl('/api/config'), {cache: 'no-store'}),
      configFetch(liveApiUrl('/api/config/capabilities'), {cache: 'no-store'})
    ]);
    if (!configResponse.ok) throw new Error('The saved configuration is unavailable.');
    activeConfig = await configResponse.json();
    configRevision = configResponse.headers.get('ETag')?.replace(/^"|"$/g, '');
    capabilities = capabilitiesResponse.ok ? await capabilitiesResponse.json() :
      {editable: false, restartAvailable: false, changesRequireRestart: true};
    if (capabilities.setupRequired && !settingsModeStored) settingsMode = 'simple';
    receiverApplyPending = ['unknown', 'saved-pending', 'partial', 'indeterminate', 'failed',
      'receiver-confirmed-config-not-saved'].includes(capabilities.receiverSynchronization?.applicationState);
    originalConfig = serializeConfig(activeConfig);
    const replayFile = new URLSearchParams(window.location.search).get('replayFile');
    if (replayFile) {
      activeConfig.capture.replay = {...activeConfig.capture.replay,
        state: true, file: replayFile, format: 'auto'};
      try { sessionStorage.setItem('blah2-settings-tab', 'capture'); } catch (_) {}
      if (window.history?.replaceState)
        window.history.replaceState(null, '', window.location.pathname);
    }
    validationErrors = [];
    validationPending = false;
    target.innerHTML = `<div class="config-toolbar"><div><strong>System settings</strong><span id="config-state">Up to date</span></div><div class="config-actions"><button id="config-reset" class="button-secondary" type="button" disabled>Discard changes</button><button id="config-save" class="button-primary" type="button" disabled>Save & Restart</button></div></div><div id="config-message" class="config-message"></div><section id="upstream-status" class="upstream-status"><div class="upstream-header"><strong>Upstream receiver check</strong><span class="upstream-badge">Checking…</span></div></section><div id="config-restart-progress" class="config-restart-progress" role="status" aria-live="polite" hidden><div class="config-restart-copy"><span id="config-restart-label">Preparing restart…</span><b id="config-restart-value">0%</b></div><div class="config-restart-track"><span id="config-restart-bar"></span></div></div><div id="config-fields" class="config-editor"></div>`;
    if (capabilities.receiverSynchronization?.pendingIntentValue === 'save-pending-v1') {
      const later = document.createElement('button');
      later.id = 'config-save-later';
      later.type = 'button';
      later.className = 'button-secondary';
      later.textContent = 'Save for later';
      later.title = 'Save without changing the receiver or restarting.';
      later.disabled = true;
      later.addEventListener('click', () => saveConfiguration('pending'));
      document.getElementById('config-save').before(later);
    }
    const setup = document.createElement('div');
    setup.className = 'config-setup-note';
    if (capabilities.setupRequired) {
      const title = document.createElement('strong');
      const copy = document.createElement('span');
      title.textContent = ' First-run setup required.';
      copy.textContent = ' Choose your receiver, verify the tuning frequency and sample rate, then review the receiver and transmitter sites before Save & Restart.';
      setup.append(title, copy);
    }
    target.querySelector('.config-toolbar').after(setup);
    const assistant = document.createElement('div');
    assistant.id = 'setup-assistant-root';
    setup.after(assistant);
    const diagnostics = document.createElement('div');
    diagnostics.id = 'config-diagnostics';
    diagnostics.setAttribute('role', 'status');
    assistant.after(diagnostics);
    const acceleration = document.createElement('div');
    acceleration.id = 'acceleration-status';
    acceleration.className = 'config-setup-note';
    acceleration.setAttribute('role', 'status');
    diagnostics.after(acceleration);
    document.getElementById('config-save').addEventListener('click', saveConfiguration);
    document.getElementById('config-reset').addEventListener('click', () => {
      if (validationTimer) window.clearTimeout(validationTimer);
      validationSequence += 1;
      validationErrors = [];
      validatedSnapshot = null;
      if (pendingLiveConfig !== null) {
        activeConfig = pendingLiveConfig;
        originalConfig = serializeConfig(pendingLiveConfig);
        configRevision = pendingRevision;
        pendingLiveConfig = null;
        showMessage('Reloaded the saved configuration.', 'success');
      } else {
        activeConfig = JSON.parse(originalConfig);
        showMessage('Changes discarded.');
      }
      renderEditor();
      scheduleValidation();
    });
    if (!capabilities.editable)
      showMessage('Settings are read-only. VectorWarp needs write access to the config file and its folder.', 'warning');
    else
      showMessage('');
    renderEditor();
    if (replayFile)
      showMessage('Recording selected for replay. Review the receiver and replay settings, then choose Save & Restart.', 'warning');
    if (window.VectorWarpReports) window.VectorWarpReports.addSettingsEntry(target, () =>
      reportContext(window.VectorWarpReports.classify(lastReportingStatus) ||
        {code: 'MANUAL_REPORT', component: 'support', stage: 'manual', severity: 'degraded',
          message: 'User requested a diagnostic report'}));
    scheduleValidation();
    refreshConfigDiagnostics();
    refreshUpstreamStatus();
    if (window.blah2ConfigSyncTimer) window.clearInterval(window.blah2ConfigSyncTimer);
    window.blah2ConfigSyncTimer = window.setInterval(syncConfigurationFromServer, 5000);
    if (window.blah2DiagnosticsTimer) window.clearInterval(window.blah2DiagnosticsTimer);
    window.blah2DiagnosticsTimer = window.setInterval(refreshConfigDiagnostics, 5000);
    if (window.blah2UpstreamStatusTimer)
      window.clearInterval(window.blah2UpstreamStatusTimer);
    window.blah2UpstreamStatusTimer = window.setInterval(
      refreshUpstreamStatus, 5000);
    if (!window.blah2ConfigSyncListeners) {
      window.blah2ConfigSyncListeners = true;
      window.addEventListener('focus', syncConfigurationFromServer);
      document.addEventListener('visibilitychange', () => {
        if (!document.hidden) syncConfigurationFromServer();
      });
    }
  } catch (error) {
    target.replaceChildren();
    const message = document.createElement('p');
    message.textContent = `Settings unavailable. ${error.message} Check that VectorWarp is running and its API port is reachable.`;
    const retry = document.createElement('button');
    retry.textContent = 'Try again';
    retry.addEventListener('click', renderConfiguration);
    target.append(message, retry);
    showReportAction(target, window.VectorWarpReports?.classifyFailure('api', {
      message: error.message || 'Settings API unavailable', details: {routeName: 'configuration'}}));
  }
}

async function refreshConfigDiagnostics() {
  if (typeof document === 'undefined') return;
  if (document.hidden) return;
  if (restartInProgress) { renderReceiverOverload(null); return; }
  const target = document.getElementById('config-diagnostics');
  if (!target) return;
  try {
    const status = await readRadarState();
    if (typeof document === 'undefined' || !target.isConnected) return;
    lastReportingStatus = status;
    renderReceiverStartup(status);
    renderReceiverOverload(status);
    const acceleration = document.getElementById('acceleration-status');
    if (acceleration) {
      acceleration.textContent = `Delay–Doppler: ${accelerationSummary(status.acceleration, status.radar)}. ` +
        `Clutter: ${accelerationSummary(status.clutterAcceleration, status.radar)}.`;
      if (status.gpuSetup?.platform === 'darwin' || status.gpuSetup?.pi || ['group-access-needed', 'unavailable'].includes(status.gpuSetup?.serviceAccess?.state)) {
        const setup = document.createElement('div');
        setup.textContent = `GPU setup: ${status.gpuSetup.message}`;
        acceleration.append(setup);
        if (status.gpuSetup.pi && !['qualified', 'partially-qualified'].includes(status.gpuSetup.state)) {
          const command = document.createElement('code');
          command.textContent = 'sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver';
          setup.append(document.createTextNode(' Run on the VectorWarp computer: '), command);
        }
        if (status.gpuSetup.serviceAccess?.state === 'group-access-needed') {
          const access = document.createElement('div');
          access.textContent = 'Allow GPU access: sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --enable-service-access';
          setup.append(access);
        }
      }
    }
    target.textContent = status.errors?.length ? status.errors.join(' ') :
      status.restart?.state === 'failed' ? status.restart.message :
      status.configRevision !== status.loadedRevision ?
        'Saved settings differ from running settings. Run vectorwarp restart to apply them.' :
        status.radar !== 'receiving' ? status.message || 'Radar data is stale. Check the processing service.' : '';
    showReportAction(target, window.VectorWarpReports?.classify(status));
  } catch (_) {
    if (typeof document === 'undefined' || !target.isConnected) return;
    renderReceiverStartup(null);
    renderReceiverOverload(null);
    target.textContent = 'Cannot read service status. Check the connection to VectorWarp.';
    showReportAction(target, window.VectorWarpReports?.classifyFailure('api', {
      message: 'Cannot read service status', details: {routeName: 'status'}}));
  }
}

function accelerationSummary(value, radar) {
  if (radar !== 'receiving') return 'Waiting for radar';
  if (!value) return 'Not reported';
  if (value.state === 'checking') return `Checking GPU: ${value.device || 'detected device'}`;
  if (typeof value.active === 'string' && value.active.startsWith('vulkan'))
    return `GPU: ${value.device}`;
  return `CPU${value.reason ? ` — ${value.reason}` : ''}`;
}

if (typeof module !== 'undefined')
  module.exports = {applyDeviceProfile, metadata, normalizeKrakenChannels, receiverFeatureCapabilities,
    receiverSpecificProcessPath, adsbExchangeId, adsbPublicSource,
    serializeConfig, upstreamRestartError, receiverSaveMessage, receiverFailureMessage, receiverMayHaveChanged,
    receiverSaveDetails, accelerationSummary, CONFIG_SAVE_TIMEOUT_MS, MEASURED_GAIN_SAVE_TIMEOUT_MS};
