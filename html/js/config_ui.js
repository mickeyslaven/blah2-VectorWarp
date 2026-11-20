const geometryEditor = typeof module !== 'undefined' && module.exports ?
  require('./kraken_geometry') : window.KrakenGeometry;
const configProfiles = typeof module !== 'undefined' && module.exports ?
  require('./config_profiles') : window.VectorWarpConfigProfiles;
const processingTopology = typeof module !== 'undefined' && module.exports ?
  require('./processing_capabilities') : window.VectorWarpProcessingCapabilities;
const CONFIG_SAVE_TIMEOUT_MS = 90000;
const MEASURED_GAIN_SAVE_TIMEOUT_MS = 1250000;
const ATSC_CHANNELIZER_DEFAULTS = Object.freeze({
  // A/53 8-VSB nominal occupied bandwidth.  The surrounding part of the
  // 6 MHz allocation is transition band, not useful carriers.
  ATSC: Object.freeze({occupiedBandwidthHz: 5380000, tapsPerPhase: 64}),
  'ATSC-1.0': Object.freeze({occupiedBandwidthHz: 5380000, tapsPerPhase: 64}),
  // A/322 6 MHz, bsr_coefficient=2, C_red=0.  Other ATSC 3 channel widths
  // remain editable; this prevents a new 6 MHz lane inheriting A/53's width.
  'ATSC-3.0': Object.freeze({occupiedBandwidthHz: 5832844, tapsPerPhase: 128})
});

function applyIlluminatorStandardDefaults(item, selected) {
  item.standard = selected;
  const profile = ATSC_CHANNELIZER_DEFAULTS[selected];
  if (!profile) return item;
  item.occupied_bandwidth_hz = profile.occupiedBandwidthHz;
  item.filter_taps_per_phase = profile.tapsPerPhase;
  item.atsc_reference_mode = 'automatic';
  return item;
}
const CONFIG_GROUP_META = {
  'capture': ["Receiver", "Tuning and sites"],
  'process': ["Radar", "Detection and tracking"],
  'network': ["Connectivity", "Ports"],
  'truth': ["ADS-B", "Aircraft data"],
  'location': ["Sites", "Receiver and illuminator"],
  'save': ["Recording", "Files and clips"],
  'save.retention': ["Storage retention", "Keep recordings bounded by age, total storage, and free disk space."],
  'capture.aircraft_clips': ["Automatic aircraft IQ clips", "Receiver-independent bounded recordings triggered by eligible ADS-B aircraft passes."],
  'capture.aircraft_clips.unmatched_motion': ["Unmatched aircraft-like motion", "Bounded full-passband clips for operational tracks whose measured motion resembles ADS-B-associated aircraft but remains unassociated."],
  'capture.device': ["Receiver hardware", "Radio, channels, and array"],
  'capture.device.array_geometry': ["Array setup", "Layout, channel map, and measurements"],
  'capture.device.heimdall': ["Radio connection & gain", "Suite connection and tuner gain"],
  'capture.replay': ["Replay", "Use a recording instead of live radio."],
  'process.performance': ["Performance", "CPU and GPU settings"],
  'process.evidence': ["Evidence collection", "Optional bounded, truth-blind research evidence."],
  'process.evidence.causal_funnel': ["Causal-funnel evidence", "Bounded truth-blind candidate records for offline analysis."],
  'process.multi_illuminator': ["Multiple illuminators", "Process several broadcast channels independently."],
  'process.multi_illuminator.illuminators.input_role_mode': ["Input roles", "Use the receiver default or select an explicit reference input."],
  'process.data': ["Frame timing", ""],
  'process.data.overlap': ["Frame overlap", "Not used by this processor. Frames do not overlap."],
  'process.ambiguity': ["Delay & Doppler", "Limits of the radar map"],
  'process.clutter': ["Clutter removal", "Reduces stationary reflections and multipath."],
  'process.reference_synthesis': ["Reference input", "Choose a dedicated transmitter antenna or let VectorWarp combine array inputs."],
  'save.complex_cutout': ["Experimental complex CAF export", "Export complex radar maps during CPU or portable GPU replay. Choose a new file; existing files are never overwritten. Export stops at the frame or size limit."],
  'process.surveillance_fusion': ["Surveillance fusion", "Combine the ambiguity maps from coherent receiver channels."],
  'process.bearing': ["Bearing output", "Publish radar-only azimuth candidates and bistatic locations on a separate opt-in interface."],
  'process.bearing.manifold_recorder': ["Experimental array calibration corpus", "Record bounded calibration evidence from a qualified coherent array; truth is never an input."],
  'process.detection': ["Detection", "Find targets in the radar map"],
  'process.detection.track_before_detect': ["Track before detect", "Accumulate subthreshold motion-consistent energy before normal tracking."],
  'process.tracker': ["Tracking", "Joins repeated detections into persistent tracks."],
  'process.tracker.initiate': ["Track confirmation", "Detections needed to start a track"],
  'process.tracker.initiate.stationary_clutter': ["Persistent clutter filter", "Blocks births and quarantines tracks dominated by recurring low-Doppler cells."],
  'process.tracker.initiate.doppler_ridge_clutter': ["Experimental Doppler-ridge birth filter", "Reject new tracks sustained by persistent Doppler ridges. Requires detection and tracking; unavailable with resident coherent or snapshot processing. History is bounded; existing tracks are unaffected. Whole-pass false-track and missed-target qualification is pending."],
  'network.ports': ["Service ports", "TCP ports used by the API and radar data streams"],
  'truth.adsb': ["ADS-B", "Aircraft overlay and radar comparison"],
  'truth.remote_id': ["Drone Remote ID", "Experimental cooperative-drone receiver."],
  'truth.ais': ["AIS (not implemented)", "Reserved maritime-feed settings; preserved but not editable."],
  'truth.ais.enabled': ["AIS evaluation", "Not implemented in this processor."],
  'truth.ais.ip': ["Reserved AIS address", "Not used by this processor."],
  'truth.ais.port': ["Reserved AIS port", "Not used by this processor."],
  'location.rx': ["Receiver site", "Antenna position used for range geometry."],
  'location.tx': ["Illuminator site", "Broadcast transmitter position used for range geometry."]
};

function applyComposite(action, refreshGeometry = false) {
  const before = geometryEditor?.context(activeConfig);
  action(activeConfig);
  if (before !== geometryEditor?.context(activeConfig)) {
    geometryEditor?.invalidate(activeConfig.capture?.device?.array_geometry);
    if (refreshGeometry) refreshGeometryEditor();
  }
  refreshSettingsFilter();
  scheduleValidation();
}

function compositeField(title, description, control, path, fieldState = null) {
  const wrapper = document.createElement('div');
  wrapper.className = 'config-field';
  if (path) wrapper.dataset.path = path;
  const copy = document.createElement('div'); copy.className = 'config-field-copy';
  const label = document.createElement('label'); label.textContent = title;
  const help = document.createElement('small'); help.textContent = description;
  help.hidden = true;
  copy.title = description;
  copy.append(label, help);
  const controls = control.matches?.('input, select') ? [control] : control.querySelectorAll('input, select');
  if (fieldState?.readOnly) control.querySelectorAll('button').forEach(button => { button.disabled = true; });
  Array.from(controls).forEach((input, index) => {
    input.disabled ||= fieldState ? fieldState.readOnly : !capabilities.editable;
    input.dataset.originalDisabled = String(input.disabled);
    if (fieldState) input.id = `config-${path.replaceAll('.', '-')}-${index}`;
    else if (!input.id) input.id = `composite-${(path || title).replace(/[^a-z0-9]+/gi, '-')}-${index}`;
    input.title = description;
    if (!index) { label.htmlFor = input.id; help.id = `${input.id}-help`; }
    input.setAttribute('aria-describedby', help.id);
    if (fieldState && !input.hasAttribute('aria-label'))
      input.setAttribute('aria-label', `${title}${controls.length > 1 ? ` ${index + 1}` : ''}`);
  });
  const error = document.createElement('small'); error.className = 'config-field-error';
  if (fieldState) error.setAttribute('aria-live', 'polite');
  wrapper.append(copy, control, error); return wrapper;
}

function compositeGroup(title, description) {
  const details = document.createElement('details'); details.className = 'config-group composite-settings';
  const summary = document.createElement('summary');
  const copy = document.createElement('span');
  const strong = document.createElement('strong'); strong.textContent = title;
  const small = document.createElement('small'); small.textContent = description;
  small.hidden = true;
  summary.title = description;
  copy.append(strong, small); summary.appendChild(copy);
  const body = document.createElement('div'); body.className = 'config-fields';
  details.append(summary, body); return {details, body};
}

function compositeTuning(title, hidden = false) {
  const details = document.createElement('details');
  details.className = 'config-group composite-tuning';
  details.hidden = hidden;
  const summary = document.createElement('summary'); summary.textContent = title;
  const fields = document.createElement('div'); fields.className = 'config-fields';
  details.append(summary, fields);
  return {details, fields};
}

function renderClutterSettings() {
  const {details, body} = compositeGroup('Clutter removal',
    'Remove the stationary direct path and fixed multipath before detection.');
  details.open = true;
  const custom = document.createElement('div'); custom.className = 'config-fields composite-custom';
  const select = selectInput([
    {value: 'off', label: 'Off'},
    {value: 'automatic', label: 'Automatic (match the radar range)'},
    {value: 'custom', label: 'Custom filter range'}
  ], configProfiles.clutterMode(activeConfig), value => {
    if (select.selectedOptions[0]?.disabled) return;
    applyComposite(config => configProfiles.applyClutterMode(config, value));
    custom.hidden = value !== 'custom';
  });
  if (activeConfig.process.multi_illuminator?.enabled === true) {
    const off = [...select.options].find(option => option.value === 'off');
    off.disabled = true;
    off.title = 'Multi-illuminator processing requires clutter removal before detection.';
  }
  body.appendChild(compositeField('Clutter removal',
    (activeConfig.process.multi_illuminator?.enabled === true ?
      'Multi-illuminator processing requires clutter removal. ' : '') +
      'Automatic follows the complete delay map. Custom is only for installations that must limit cancellation.',
    select, 'process.clutter.enable'));
  custom.append(field(activeConfig.process.clutter.delayMin, ['process', 'clutter', 'delayMin']),
    field(activeConfig.process.clutter.delayMax, ['process', 'clutter', 'delayMax']));
  custom.hidden = configProfiles.clutterMode(activeConfig) !== 'custom';
  body.appendChild(custom); return details;
}

function renderDetectionSettings() {
  const detection = activeConfig.process.detection;
  const {details, body} = compositeGroup('Detection',
    'Choose the false-alarm tradeoff, then optionally customize the CFAR estimator.');
  details.open = true;
  body.appendChild(field(detection.enable, ['process', 'detection', 'enable']));
  const sensitivity = selectInput([
    {value: 'conservative', label: 'Fewer false alarms'},
    {value: 'balanced', label: 'Balanced (recommended)'},
    {value: 'sensitive', label: 'Weak targets (more false alarms)'},
    {value: 'research', label: 'Maximum sensitivity — research (many false alarms)'},
    {value: 'custom', label: 'Custom'}
  ], configProfiles.detectionSensitivity(activeConfig), value => {
    if (value !== 'custom') applyComposite(config => configProfiles.applyDetectionSensitivity(config, value));
  });
  body.appendChild(compositeField('Detection sensitivity',
    'Changes the whole-bank false-alarm probability. Weak targets admits more noise; it does not create signal that is absent.',
    sensitivity));
  const {details: tuning, fields} = compositeTuning('CFAR tuning');
  for (const key of ['pfa', 'nGuard', 'nTrain', 'nCentroid']) fields.appendChild(field(detection[key], ['process', 'detection', key]));
  body.appendChild(tuning); return details;
}

function delaySampleDescription(config) {
  const multi = config.process?.multi_illuminator;
  const lanes = multi?.enabled === true ?
    (multi.illuminators || []).filter(lane => lane?.enabled === true) : [];
  const distance = rate => Number.isFinite(Number(rate)) && Number(rate) > 0 ?
    `${(299792.458 / Number(rate)).toFixed(3)} km` : 'unavailable';
  const reference = config.process?.delay_reference_sample_rate_hz;
  const prefix = reference === undefined ? 'Legacy lane-local delay bins. ' :
    `Configured delay bins represent ${distance(reference)} each at ${(reference / 1e6).toFixed(3)} MS/s; lane limits scale automatically. `;
  if (reference !== undefined && lanes.length && config.process?.ambiguity) {
    const windows = lanes.map(lane => {
      const effective = configProfiles.effectiveDelayConfig(config, lane.output_sample_rate_hz,
        {scaleNeighborhoods: true}).process;
      const km = bin => (bin * 299792.458 / lane.output_sample_rate_hz).toFixed(3);
      return `${lane.id}: ${effective.ambiguity.delayMin}–${effective.ambiguity.delayMax} bins ` +
        `(${km(effective.ambiguity.delayMin)}–${km(effective.ambiguity.delayMax)} km), detection from ` +
        `${km(effective.detection.minDelay)} km`;
    });
    return prefix + windows.join('; ') + '.';
  }
  return prefix + (lanes.length ? `One delay sample in each lane is ${lanes.map(lane =>
    `${distance(lane.output_sample_rate_hz)} (${lane.id})`).join(', ')}.` :
    `One delay sample is ${distance(config.capture?.fs)}.`);
}

function renderInterferenceRejectionSettings() {
  const detection = activeConfig.process.detection;
  const {details, body} = compositeGroup('Nearby and stationary rejection',
    'Exclude the direct-path neighborhood and near-zero Doppler from target detection.');
  body.appendChild(compositeField('Delay scale',
    'Fixed distance keeps the search and rejection distances when sample rates change.',
    selectInput([{value: 'physical', label: 'Fixed physical distance'},
      {value: 'legacy', label: 'Legacy lane-local sample bins'}],
    activeConfig.process.delay_reference_sample_rate_hz === undefined ? 'legacy' : 'physical',
    value => applyComposite(config => {
      if (value === 'legacy') delete config.process.delay_reference_sample_rate_hz;
      else config.process.delay_reference_sample_rate_hz =
        configProfiles.legacyDelayReferenceRate(config) ||
        config.process.multi_illuminator?.illuminators?.find(lane => lane?.enabled === true)?.output_sample_rate_hz ||
        config.capture.fs;
    }))));
  if (activeConfig.process.delay_reference_sample_rate_hz !== undefined)
    body.appendChild(field(activeConfig.process.delay_reference_sample_rate_hz,
      ['process', 'delay_reference_sample_rate_hz']));
  body.appendChild(compositeField('Minimum excess path',
    `Detections closer than this are ignored. ${delaySampleDescription(activeConfig)} Distances are transmitter→target→receiver excess path, not receiver slant range.`,
    typedInput(detection.minDelay, ['process', 'detection', 'minDelay']), 'process.detection.minDelay'));
  body.appendChild(compositeField('Near-zero Doppler exclusion',
    'Ignore detections inside ± this frequency. Higher values suppress stationary clutter but can remove slow radial motion.',
    typedInput(detection.minDoppler, ['process', 'detection', 'minDoppler']), 'process.detection.minDoppler'));
  return details;
}

function renderWeakTargetSettings() {
  const detection = activeConfig.process.detection;
  const available = processingControlCapabilities();
  const {details, body} = compositeGroup('Multi-CPI integration and track-before-detect',
    'Accumulate target power across processing intervals before conventional tracking. Longer banks add latency, trajectory hypotheses, and processing work.');
  const choices = [{value: 'off', label: 'Off (single frame)'},
    {value: 'two', label: 'Two-frame integration'},
    {value: 'two_motion', label: 'Two-frame integration + motion search'},
    {value: 'four', label: 'Four-frame track-before-detect'},
    {value: 'eight', label: 'Eight-frame track-before-detect'},
    {value: 'custom', label: 'Custom'}];
  const select = selectInput(choices, configProfiles.weakTargetMode(activeConfig), value => {
    if (select.selectedOptions[0]?.disabled) return;
    if (value !== 'custom') {
      applyComposite(config => configProfiles.applyWeakTargetMode(config, value));
      renderEditor();
    }
  });
  select.dataset.processingControl = 'recovery-mode';
  body.appendChild(compositeField('Integration / accumulation mode', available.integration ?
    `Two-, four-, and eight-frame modes integrate power${available.laneIntegration ? ' independently in each illuminator lane' : ''} on CPU or qualified GPU paths. Overlapping source CPIs do not count as fresh confirmation.` :
    'Multi-frame recovery is unavailable in this build for the selected input topology.', select));
  const {details: custom, fields} = compositeTuning('Recovery tuning');
  fields.append(field(detection.integration_cpis, ['process', 'detection', 'integration_cpis']),
    field(detection.integration_motion_bins, ['process', 'detection', 'integration_motion_bins']));
  if (detection.track_before_detect)
    fields.appendChild(group('track_before_detect', detection.track_before_detect, ['process', 'detection']));
  body.appendChild(custom); return details;
}

function renderTrackerSettings() {
  const tracker = activeConfig.process.tracker;
  const {details, body} = compositeGroup('Tracking',
    'Turn repeated detections into persistent tracks without changing detector sensitivity.');
  details.open = true;
  body.appendChild(field(tracker.enable, ['process', 'tracker', 'enable']));
  body.appendChild(field(tracker.profile ?? 'balanced', ['process', 'tracker', 'profile']));
  const {details: tuning, fields} = compositeTuning('Track confirmation and lifetime');
  for (const key of ['M', 'N', 'min_snr_db', 'maxAcc', 'acceleration_model'])
    if (tracker.initiate?.[key] !== undefined) fields.appendChild(field(tracker.initiate[key], ['process', 'tracker', 'initiate', key]));
  fields.appendChild(field(tracker.delete, ['process', 'tracker', 'delete']));
  body.appendChild(tuning);
  const ridge = tracker.initiate?.doppler_ridge_clutter;
  if (ridge && !experimentalSettingPath('process.tracker.initiate.doppler_ridge_clutter') && !capabilities.fieldRules?.[
    'process.tracker.initiate.doppler_ridge_clutter.enable']?.readOnly) {
    const {details: ridgeDetails, fields: ridgeFields} =
      compositeTuning('Persistent Doppler-ridge interference');
    for (const key of ['enable', 'persistence_cpis', 'doppler_tolerance_hz',
      'minimum_delay_separation_km'])
      if (ridge[key] !== undefined)
        ridgeFields.appendChild(field(ridge[key],
          ['process', 'tracker', 'initiate', 'doppler_ridge_clutter', key]));
    body.appendChild(ridgeDetails);
  }
  return details;
}

function renderPerformanceSettings() {
  const performance = activeConfig.process.performance || {};
  const {details, body} = compositeGroup('Processing',
    'Automatic prefers an available qualified GPU and safely falls back to CPU.');
  const select = selectInput([{value: 'automatic', label: `Automatic (${performance.acceleration === 'gpu-resident' ? 'GPU resident' : performance.acceleration === 'gpu' ? 'GPU with fallback' : 'best available'})`},
    {value: 'cpu', label: 'CPU only (free the GPU)'}], configProfiles.processingMode(activeConfig), value => {
    applyComposite(config => configProfiles.applyProcessingMode(config, value));
  });
  body.appendChild(compositeField('Processing hardware',
    'Hardware selection preserves radar settings. Unsupported combinations must be corrected before saving.',
    select, 'process.performance.acceleration'));
  const {details: tuning, fields} = compositeTuning('CPU scheduling and queue');
  for (const key of ['surveillance_workers', 'fft_threads'])
    if (performance[key] !== undefined) fields.appendChild(field(performance[key], ['process', 'performance', key]));
  if (activeConfig.process.data?.buffer !== undefined)
    fields.appendChild(field(activeConfig.process.data.buffer, ['process', 'data', 'buffer']));
  body.appendChild(tuning);
  return details;
}

function appendCompositeRadarSettings(content) {
  content.append(renderClutterSettings(), renderDetectionSettings(),
    renderInterferenceRejectionSettings(), renderWeakTargetSettings(),
    renderTrackerSettings(), renderPerformanceSettings());
}

let activeConfig = null;
let originalConfig = '';
let originalConfigObject = null;
let capabilities = {};
let configurationOperation = 'idle';
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
const configurationBusy = () => configurationOperation !== 'idle';
let upstreamPending = false;
let restartRetry = false;
let receiverApplyPending = false;
let replayLaunchPending = false;
let recentSites = [];
let receiverInvalidatedGeometry = false;
let lastDetectedReceiverTypes = new Set();
let settingsSearch = '';
let settingsEditedOnly = false;
let refreshSettingsFilter = () => {};
let activateSettingsTab = () => {};
const PRIMARY_SETTING_PATHS = new Set([
  'capture.fc', 'capture.fs', 'capture.device.type', 'capture.device.channel_count',
  'capture.device.heimdall.gain_control', 'capture.device.bandwidthNumber',
  'capture.device.agcSetPoint', 'capture.device.gain', 'capture.device.gain_lna',
  'capture.device.gain_vga', 'process.data.cpi',
  'process.clutter.enable', 'process.detection.enable', 'process.detection.pfa',
  'process.detection.minDelay', 'process.detection.minDoppler',
  'process.detection.integration_cpis', 'process.tracker.enable',
  'process.tracker.profile', 'process.performance.acceleration',
  'truth.adsb.enabled', 'truth.adsb.tar1090', 'save.path', 'network.ip'
]);
const EXPERIMENTAL_SETTING_PREFIXES = [
  'truth.remote_id',
  'process.multi_illuminator',
  'process.bearing.manifold_recorder',
  'process.reference_synthesis.target_protection_enabled',
  'process.reference_synthesis.target_protection_beams',
  'process.reference_synthesis.target_protection_beam_index',
  'save.complex_cutout',
  'process.tracker.initiate.doppler_ridge_clutter',
];
function experimentalSettingPath(path) {
  return EXPERIMENTAL_SETTING_PREFIXES.some(prefix => path === prefix || path.startsWith(`${prefix}.`));
}

function exposeLegacyTrackerProfile(config) {
  const tracker = config?.process?.tracker;
  if (!tracker || tracker.profile !== undefined) return config;
  // Released configurations predate profiles and are behaviorally permissive.
  // Make that safe legacy choice visible without changing it merely by opening
  // Settings. A later explicit save persists the selected profile.
  tracker.profile = 'permissive';
  if (tracker.initiate && tracker.initiate.min_snr_db === undefined)
    tracker.initiate.min_snr_db = 7;
  return config;
}
function refreshSetupAssistant() {
  const target = document.getElementById('setup-assistant-root');
  if (!target || !window.VectorWarpSetupAssistant || !activeConfig) return;
  window.VectorWarpSetupAssistant.render(target, {
    getConfig: () => activeConfig,
    openTab(tab) {
      activateSettingsTab(tab);
      document.getElementById('config-fields')?.scrollIntoView({
        behavior: window.matchMedia?.('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth'
      });
    },
    set: setValue, refresh: renderEditor, refreshEditor: renderEditor,
    receiverProfiles: () => capabilities.deviceProfiles || [],
    switchReceiver: switchDevice,
    discoverReceivers: async () => {
      const response = await configFetch(liveApiUrl('/api/receivers/discover'), {
        method: 'POST', headers: {'Content-Type': 'application/json',
          'X-VectorWarp-Intent': 'receiver-discovery-v2'}, body: '{}'
      }, 30000);
      const result = await response.json();
      if (!response.ok) throw new Error(result.message || 'Receiver discovery failed.');
      return (result.receivers || []).filter(receiver =>
        receiver.capabilities?.detected === true &&
        (capabilities.deviceProfiles || []).some(profile => profile.type === receiver.type));
    },
    saveStep: () => saveConfiguration('pending'),
    save: () => saveConfiguration()
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
  if (!capabilities.editable || configurationBusy()) return;
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
  const channelTransmitters = activeConfig.process?.multi_illuminator?.enabled === true ?
    (activeConfig.process.multi_illuminator.illuminators || [])
      .filter(lane => lane?.enabled === true && siteValue(lane.transmitter)) : [];
  for (const [role, title] of [['rx', 'Receiver'], ['tx', 'Transmitter']]) {
    const perChannel = role === 'tx' && channelTransmitters.length > 0;
    const row = document.createElement('div');
    row.className = 'config-field site-setting';
    row.dataset.path = `location.${role}`;
    const copy = document.createElement('div');
    copy.className = 'config-field-copy';
    const label = document.createElement('label');
    label.textContent = perChannel ? 'Channel transmitters' : title;
    label.htmlFor = `site-select-${role}`;
    const rawSite = activeConfig.location[role];
    const current = siteValue(rawSite);
    const named = typeof rawSite?.name === 'string' && rawSite.name.trim() &&
      !/^Set (receiver|transmitter) site$/.test(rawSite.name);
    const coordinates = document.createElement('small');
    coordinates.textContent = perChannel ?
      `${channelTransmitters.length} enabled · ${channelTransmitters.map(lane => lane.id).join(', ')}` :
      current ? `${current.latitude}°, ${current.longitude}° · ${current.altitude} m` :
      named ? 'Check coordinates' : 'Not set';
    copy.append(label, coordinates);
    const controls = document.createElement('div');
    controls.className = 'site-controls';
    const select = document.createElement('select');
    select.id = label.htmlFor;
    select.setAttribute('aria-label', perChannel ? 'Channel transmitter locations' : `${title} location`);
    select.disabled = perChannel || !capabilities.editable;
    select.dataset.originalDisabled = String(select.disabled);
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = perChannel ? 'Configured per channel' :
      !current && named ? rawSite.name : 'Choose a location';
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
      button.disabled = perChannel || !capabilities.editable || (action === 'edit' && !current && !named);
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


function words(value) {
  return value.replace(/([a-z0-9])([A-Z])/g, '$1 $2')
    .replace(/_/g, ' ').replace(/^./, letter => letter.toUpperCase());
}

function metadata(path, receiverType = activeConfig?.capture?.device?.type, fieldRules = capabilities.fieldRules) {
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
  const presentation = fieldRules?.[key]?.presentation || CONFIG_GROUP_META[key];
  if (presentation) return presentation;
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
  applyComposite(() => {
    let target = activeConfig;
    path.slice(0, -1).forEach(key => { target = target[key]; });
    target[path[path.length - 1]] = value;
    if (value === undefined && /^capture\.replay\.(recording|acquisition)_sha256$/.test(path.join('.')))
      delete target[path[path.length - 1]];
    if (value === false && path.join('.') === 'process.detection.track_before_detect.enable') {
      const tbd = activeConfig.process.detection.track_before_detect;
      if (tbd.fractional_delay !== undefined) tbd.fractional_delay = false;
      // Reflect the operator's explicit disable action; loading settings never
      // clears saved, currently unavailable values.
      renderEditor();
    }
    if ((value === true && (path.join('.') === 'process.bearing.enable' ||
      path.join('.') === 'process.detection.track_before_detect.enable' &&
        activeConfig.process.tracker?.beam_tolerance !== undefined)) ||
      (path.join('.') === 'process.surveillance_fusion.mode' && value === 'noise_whitened_beam_bank'))
      configProfiles.applyProcessingMode(activeConfig, 'automatic');
    if (path.join('.') === 'capture.device.heimdall.gain_control' && value === 'measured') {
      delete activeConfig.capture.device.heimdall.channel_gains;
      document.querySelector('[data-path="capture.device.heimdall.channel_gains"]')?.remove();
    }
    if (path.join('.') === 'capture.fc' && activeConfig.capture?.device?.type === 'RspDuo') {
      const control = document.getElementById('config-capture-device-lnaState-0');
      if (control) {
        const next = rspDuoLnaInput(activeConfig.capture.device.lnaState);
        control.replaceChildren(...Array.from(next.childNodes));
      }
    }
  }, true);
}

function normalizeKrakenChannels(config, previousCount = null) {
  if (config.capture?.device?.type !== 'Kraken') return config;
  const device = config.capture.device;
  geometryEditor?.invalidate(device.array_geometry);
  const synthesis = config.process?.reference_synthesis;
  const count = device.channel_count;
  if (!Number.isInteger(count) || count < 2 || count > 8 || !synthesis)
    return config;
  // Preserve explicit channel identities. Invalid/out-of-range mappings remain
  // visible for repair instead of selecting a different physical reference.
  return config;
}

function applyDeviceProfile(config, profile) {
  const initializeProcessing = config.capture?.device?.type === 'Unconfigured';
  if (config.capture?.replay?.state !== true)
    geometryEditor?.invalidate(config.capture?.device?.array_geometry);
  return configProfiles.applyReceiverProfile(config, profile, initializeProcessing);
}

function receiverFeatureCapabilities(type, profiles = []) {
  return profiles.find(profile => profile.type === type)?.features || {};
}

function processingControlCapabilities(config = activeConfig, backends = capabilities.processingBackends) {
  const topology = processingTopology.processingCapabilities(config);
  const process = config.process || {}, tracker = process.tracker || {};
  const stage = backends?.stages?.resident;
  const recorded = config.capture?.replay?.state === true;
  const lanes = process.multi_illuminator?.enabled === true ?
    (process.multi_illuminator.illuminators || []).filter(lane => lane?.enabled === true) : [];
  const laneIntegration = lanes.length > 0;
  const laneSites = lanes.every(lane => {
    const site = lane.transmitter_location;
    return site && Number.isFinite(site.latitude) && Math.abs(site.latitude) <= 90 &&
      Number.isFinite(site.longitude) && Math.abs(site.longitude) <= 180 && Number.isFinite(site.altitude) &&
      !(site.latitude === config.location?.rx?.latitude && site.longitude === config.location?.rx?.longitude);
  });
  const laneArrayRoles = lanes.every(lane => lane.reference_channel === undefined &&
    lane.surveillance_channels === undefined);
  const coherentLanes = laneSites && laneArrayRoles;
  const residentReference = topology.validTopology && processingTopology.residentReferenceCompatible(topology);
  const resident = stage?.selectable === true && residentReference && process.clutter?.enable === true;
  const regular = processingTopology.regularSurveillancePolygon(
    config.capture?.device?.array_geometry, topology.surveillanceChannels);
  const coherentGeometry = topology.coherentSurveillance && topology.surveillanceGeometry && topology.confirmedGeometry;
  const coherentFusion = ['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(process.surveillance_fusion?.mode);
  const residentArray = resident && coherentGeometry && regular && coherentLanes;
  const detection = process.detection?.enable === true;
  const tbd = topology.validTopology && detection &&
    (tracker.beam_tolerance === undefined || residentArray && coherentFusion);
  const manifold = !laneIntegration && coherentGeometry && coherentFusion && coherentLanes && topology.appliedGainMetadata &&
    detection && config.truth?.adsb?.enabled === true;
  const nativeGeometry = [config.capture?.fs,config.capture?.fc,process.data?.cpi]
    .every(value=>Number.isFinite(value) && value>0) &&
    lanes.every(lane => [lane.frequency_hz,lane.output_sample_rate_hz]
      .every(value=>Number.isFinite(value) && value>0));
  const powerReason = !topology.validTopology ? 'Recovery requires valid reference and surveillance input roles.' :
    !detection ? 'Enable detection to use power integration and recovery.' :
    tracker.enable !== true ? 'Enable tracking to use stationary suppression.' :
    !process.detection?.track_before_detect?.enable ? 'Enable track-before-detect before fractional alignment.' :
    !nativeGeometry ? 'Fractional alignment requires valid sample rate, carrier frequency, and CPI duration.' :
    'The configured temporal processing is available.';
  const laneReason = !laneSites ?
    'Coherent lanes require explicit transmitter latitude, longitude, and altitude, distinct from the receiver, for every enabled lane; the global transmitter is never reused.' :
    'Coherent lanes must inherit matching array-eigenbeam reference and surveillance inputs; dedicated per-lane overrides are incompatible.';
  const reason = !coherentLanes ? laneReason :
    stage?.selectable !== true ? `GPU-resident processing unavailable${stage?.reason ? `: ${stage.reason}` : ' on the current backend'}.` :
    !resident ? 'Resident controls require enabled clutter and valid input roles.' :
    !residentArray || !coherentFusion ? 'Coherent resident features require confirmed regular-polygon geometry and coherent array fusion.' :
    !detection || tracker.enable !== true ? 'Enable detection and tracking to use resident recovery.' :
    !process.detection?.track_before_detect?.enable ? 'Enable track-before-detect before fractional alignment.' :
    'The configured temporal processing is available.';
  return {topology, recorded, laneIntegration, coherentLanes, laneReason, resident, residentArray, tbd,
    manifold,
    manifoldRecorder: manifold && Number.isInteger(config.network?.ports?.manifold) &&
      config.network.ports.manifold > 0 && config.network.ports.manifold <= 65535,
    projectedBearing: residentArray && topology.channelCount === 5 && process.bearing?.enable === true &&
      process.bearing?.method === 'covariance_mvdr' && process.bearing?.manifold_file === undefined &&
      process.surveillance_fusion?.manifold_file === undefined &&
      lanes.every(lane => lane.detection_manifold_file === undefined && lane.bearing_manifold_file === undefined) &&
      process.surveillance_fusion?.mode === 'coherent_beam_bank' &&
      (process.surveillance_fusion?.direct_path_null_db ?? 0) === 0,
    arrayEditor: topology.liveMchq || topology.coherentSurveillance,
    integration: topology.validTopology && detection &&
      (tracker.beam_tolerance === undefined || residentArray && coherentFusion),
    stationary: topology.validTopology && detection && tracker.enable === true,
    capacity: resident && detection && tracker.enable === true,
    beamTolerance: coherentGeometry && coherentFusion && coherentLanes && tracker.enable === true &&
      ((process.detection?.integration_cpis ?? 1) <= 1 || residentArray),
    fractional: tbd && nativeGeometry && process.detection?.track_before_detect?.enable === true,
    coherentFusion: coherentGeometry && coherentLanes && (process.performance?.acceleration !== 'gpu-resident' || regular),
    bearing: residentArray && coherentFusion && detection && tracker.enable === true,
    reason, powerReason};
}

function experimentalSettingCapability(path, config = activeConfig) {
  const available = processingTopology.experimentalSettingSupport(config);
  if (path.startsWith('process.reference_synthesis.target_protection_'))
    return {available: available.targetProtection,
    reason: 'Target protection requires single-lane coherent-beam-bank replay, confirmed geometry, and explicit CPU or portable GPU; AUTO/resident reference does not implement this statistic.'};
  if (path.startsWith('save.complex_cutout.')) return {available: available.complexCutout,
    reason: 'Complex CAF export requires replay and explicit CPU or portable GPU. AUTO/resident routes do not expose the required complex maps.'};
  if (path.startsWith('process.tracker.initiate.doppler_ridge_clutter.'))
    return {available: available.ridgeBirth,
      reason: 'Doppler-ridge birth filtering requires enabled detection and host tracking; resident products do not implement it.'};
  return null;
}

function updateProcessingControlAvailability(root = document.getElementById('config-fields')) {
  if (!root || !activeConfig) return;
  const available = processingControlCapabilities();
  const permits = path => experimentalSettingCapability(path)?.available ?? (
    path.startsWith('process.bearing.manifold_recorder.') ? available.manifoldRecorder :
    path.startsWith('process.reference_synthesis.') ? available.topology.coherentSurveillance :
    path.startsWith('process.detection.track_before_detect.') ?
    (path.endsWith('.fractional_delay') ? available.fractional : available.tbd) :
    path.startsWith('process.tracker.initiate.stationary_clutter.') ? available.stationary :
    path === 'process.tracker.gpu_capacity' ? available.capacity :
    path === 'process.tracker.beam_tolerance' ? available.beamTolerance :
    path.startsWith('process.bearing.') ? available.bearing :
    ['process.detection.integration_cpis', 'process.detection.integration_motion_bins'].includes(path) ? available.integration : null);
  const reasonFor = path => experimentalSettingCapability(path)?.reason || (path.startsWith('process.bearing.manifold_recorder.') ?
      (available.laneIntegration ? 'Live calibration capture is single-lane; use RF/rate/site-matched calibration files for each illuminator.' :
        'Array calibration requires coherent detection, applied gain metadata, ADS-B truth, and a configured manifold port for recording.') :
    (['process.detection.integration_cpis', 'process.detection.integration_motion_bins'].includes(path) ||
      path.startsWith('process.detection.track_before_detect.')) &&
      activeConfig.process.tracker?.beam_tolerance !== undefined ?
      'Beam-conditioned integration requires selectable GPU-resident coherent array processing.' :
    path.startsWith('process.detection.track_before_detect.') ||
      path.startsWith('process.tracker.initiate.stationary_clutter.') ? available.powerReason :
    path.startsWith('process.reference_synthesis.') ?
      'Array reference tuning requires matching coherent reference and surveillance inputs.' : available.reason);
  let unavailable = false, firstReason = null;
  const disable = (control, denied, reason = available.reason) => {
    if (control.dataset.capabilityBaseDisabled === undefined) {
      control.dataset.capabilityBaseDisabled = String(control.disabled);
      control.dataset.capabilityBaseTitle = control.title;
    }
    control.disabled = denied || control.dataset.capabilityBaseDisabled === 'true';
    if (control.tagName !== 'OPTION') control.dataset.originalDisabled = String(control.disabled);
    if (denied) {
      unavailable = true;
      firstReason ||= reason;
      control.title = reason;
      if (control.tagName !== 'OPTION') {
        const description = (control.getAttribute('aria-describedby') || '').split(' ').filter(Boolean);
        if (!description.includes('processing-capability-note')) description.push('processing-capability-note');
        control.setAttribute('aria-describedby', description.join(' '));
      }
    } else {
      control.title = control.dataset.capabilityBaseTitle;
      const description = (control.getAttribute('aria-describedby') || '').split(' ')
        .filter(id => id && id !== 'processing-capability-note');
      if (description.length) control.setAttribute('aria-describedby', description.join(' '));
      else control.removeAttribute('aria-describedby');
    }
  };
  root.querySelectorAll('.config-field[data-path]').forEach(item => {
    const permitted = permits(item.dataset.path);
    if (permitted !== null) {
      item.classList.toggle('config-field-unavailable', !permitted);
      item.querySelectorAll('input,select,button').forEach(control => disable(control, !permitted, reasonFor(item.dataset.path)));
    }
  });
  root.querySelectorAll('[data-processing-control="recovery-mode"] option').forEach(option => {
    disable(option, ['two', 'two_motion'].includes(option.value) ? !available.integration :
      ['four', 'eight'].includes(option.value) ? !available.tbd : false,
      ['four','eight'].includes(option.value) ? available.powerReason : available.reason);
  });
  root.querySelectorAll('[data-path="process.surveillance_fusion.mode"] option').forEach(option => {
    disable(option, option.value === 'noise_whitened_beam_bank' ? !available.residentArray :
      option.value === 'coherent_beam_bank' ? !available.coherentFusion : false,
      !available.coherentLanes ? available.laneReason : available.reason);
  });
  root.querySelectorAll('[data-path="process.bearing.reference_anchor"] option[value="fixed_site_projected"]').forEach(option =>
    disable(option, !available.projectedBearing,
      'Experimental projected bearing requires five channels, confirmed lane geometry, resident covariance bearing, ideal coherent fusion with zero null, and no measured manifold files.'));
  let note = root.querySelector('#processing-capability-note');
  if (!unavailable) { note?.remove(); return; }
  if (!note) {
    note = document.createElement('p'); note.id = 'processing-capability-note';
    note.className = 'config-capability-note'; root.prepend(note);
  }
  note.textContent = `${firstReason} Saved values are preserved.`;
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

function declaredSelect(value, path, rule, control) {
  const profiles = capabilities.deviceProfiles || [];
  const sources = {
    receivers: () => profiles.map(profile => profile.type),
    channels: () => Array.from({length: activeConfig.capture.device.channel_count}, (_, index) => index),
    surveillance: () => Array.from({length: processingControlCapabilities().topology.surveillanceChannels.length + 1}, (_, index) => index)
  };
  const choices = control.choices === true ? rule.choices :
    Array.isArray(control.choices) ? control.choices : sources[control.choices]();
  const values = [...choices];
  if (!values.some(choice => String(choice) === String(value))) values.unshift(value);
  return selectInput(values.map(choice => ({value: choice, label:
    control.unit ? (choice === 0 ? control.zero : `${choice} ${control.unit}`) :
    control.labels ? (control.labels[choice] || (control.effect === 'receiver' ?
      profiles.find(profile => profile.type === choice)?.label || words(String(choice)) : String(choice))) :
    choice === 0 && control.zero ? control.zero : words(String(choice))})), value, selected => {
    if (control.effect === 'receiver') return switchDevice(selected);
    const previousCount = activeConfig.capture.device.channel_count;
    setValue(path, rule.type === 'number' ? Number(selected) : selected);
    if (control.effect === 'channels' || control.effect === 'reference') {
      normalizeKrakenChannels(activeConfig, control.effect === 'channels' ? previousCount : null);
      renderEditor();
    }
  });
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
  if (key === 'process.multi_illuminator.illuminators')
    return illuminatorListInput(value, path);
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
    const referenceChannels = activeConfig.process?.reference_synthesis?.channels ||
      [activeConfig.capture?.device?.reference_channel ?? 0];
    const channelRole = referenceChannels.includes(index) ? 'Reference' : 'Surveillance';
    caption.textContent = key === 'capture.device.heimdall.channel_gains' ? `Channel ${index}` : value.length === 2 ?
      channelRole : `Item ${index + 1}`;
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

function illuminatorListInput(value, path) {
  const wrapper = document.createElement('div');
  wrapper.className = 'illuminator-list';
  let items = JSON.parse(JSON.stringify(value));
  const anchor = document.createElement('input');
  anchor.type = 'hidden';
  const commit = () => setValue(path, JSON.parse(JSON.stringify(items)));
  const render = () => {
    anchor.value = String(items.length);
    wrapper.replaceChildren(anchor);
    const grouping = activeConfig.process.multi_illuminator.transmitter_site_grouping || 'automatic';
    const groupingLabel = document.createElement('label');
    groupingLabel.textContent = 'Transmitter-site grouping';
    const groupingSelect = selectInput([
      {value: 'automatic', label: 'Automatic with site IDs (recommended)'},
      {value: 'explicit', label: 'Require site IDs'}], grouping, selected => {
      activeConfig.process.multi_illuminator.transmitter_site_grouping = selected;
      setValue(['process', 'multi_illuminator', 'transmitter_site_grouping'], selected);
      renderEditor();
    });
    groupingSelect.title = 'Channels with the same site ID share one transmitter geometry. Different IDs remain independent. Automatic mode uses coordinate proximity only when an ID is blank.';
    groupingLabel.appendChild(groupingSelect); wrapper.appendChild(groupingLabel);
    const radiusLabel = document.createElement('label');
    radiusLabel.textContent = 'Automatic co-location radius (m)';
    const radius = document.createElement('input');
    radius.type = 'number'; radius.min = '0'; radius.max = '10000'; radius.step = '1';
    radius.value = activeConfig.process.multi_illuminator.co_location_radius_m ?? 100;
    radius.disabled = grouping === 'explicit';
    radius.title = 'Used only for channels whose transmitter site ID is blank.';
    radius.addEventListener('input', () => setValue(['process', 'multi_illuminator',
      'co_location_radius_m'], Number(radius.value)));
    radiusLabel.appendChild(radius); wrapper.appendChild(radiusLabel);
    const siteSummary = document.createElement('p');
    const enabledSites = new Map();
    items.filter(item => item.enabled === true).forEach(item => {
      const id = String(item.transmitter?.site_id || '').trim();
      const key = id ? `site:${id}` : `unassigned:${item.id}`;
      if (!enabledSites.has(key)) enabledSites.set(key, []);
      enabledSites.get(key).push(item.id);
    });
    siteSummary.className = 'config-field-help transmitter-site-summary';
    siteSummary.textContent = [...enabledSites].map(([site, lanes]) =>
      `${site.replace(/^site:/, '')}: ${lanes.join(', ')}`).join(' · ') || 'No enabled channels';
    wrapper.appendChild(siteSummary);
    items.forEach((item, index) => {
      const card = document.createElement('fieldset');
      card.className = 'illuminator-card';
      const legend = document.createElement('legend');
      legend.textContent = item.id || `Illuminator ${index + 1}`;
      card.appendChild(legend);
      const addText = (label, key, transform = value => value, optional = false) => {
        const control = document.createElement('label');
        control.textContent = label;
        const input = document.createElement('input');
        input.dataset.laneField = key;
        input.value = key.split('.').reduce((target, part) => target?.[part], item) ?? '';
        input.addEventListener('input', () => {
          const parts = key.split('.'); let target = item;
          parts.slice(0, -1).forEach(part => { target[part] ||= {}; target = target[part]; });
          if (optional && input.value === '') {
            delete target[parts.at(-1)];
            if (parts.length === 2 && Object.keys(target).length === 0) delete item[parts[0]];
          } else target[parts.at(-1)] = transform(input.value);
          if (key === 'id') legend.textContent = input.value || `Illuminator ${index + 1}`;
          commit();
        });
        control.appendChild(input); card.appendChild(control);
        return input;
      };
      const addNumber = (label, key, scale = 1) => addText(label, key,
        input => input === '' ? null : Number(input) * scale);
      const enabled = document.createElement('label');
      enabled.className = 'illuminator-enabled';
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox'; checkbox.checked = item.enabled === true;
      checkbox.addEventListener('change', () => {
        item.enabled = checkbox.checked; commit(); renderEditor();
      });
      enabled.append(checkbox, document.createTextNode('Enabled')); card.appendChild(enabled);
      addText('ID', 'id');
      const standardLabel = document.createElement('label');
      standardLabel.textContent = 'Standard';
      const standard = selectInput(['AM', 'FM', 'DAB', 'DRM', 'ATSC', 'ATSC-1.0', 'ATSC-3.0',
        'DVB-T/T2', 'ISDB-T', 'DTMB', 'CUSTOM'], item.standard,
        selected => { applyIlluminatorStandardDefaults(item, selected); commit(); renderEditor(); });
      standard.title = 'ATSC defaults use the matching US 6 MHz waveform; review bandwidth for 7 or 8 MHz ATSC 3 deployments.';
      standardLabel.appendChild(standard); card.appendChild(standardLabel);
      const atscReferenceLabel = document.createElement('label');
      atscReferenceLabel.textContent = 'ATSC reference conditioning';
      const atscReferenceMode = selectInput(['automatic', 'raw', 'pilot_aided'],
        item.atsc_reference_mode ||
          (['ATSC', 'ATSC-1.0', 'ATSC-3.0'].includes(item.standard) ?
            'automatic' : 'raw'), selected => {
          item.atsc_reference_mode = selected; commit();
        });
      const atsc1 = item.standard === 'ATSC' || item.standard === 'ATSC-1.0';
      const atsc3 = item.standard === 'ATSC-3.0';
      atscReferenceMode.disabled = !(atsc1 || atsc3);
      atscReferenceMode.title = atsc1 || atsc3 ?
        'Automatic compares each channel with its own raw control and fails closed to raw.' :
        'ATSC reference conditioning applies only to ATSC channels.';
      for (const option of atscReferenceMode.options)
        if (option.value === 'pilot_aided') option.disabled = true;
      atscReferenceLabel.appendChild(atscReferenceMode); card.appendChild(atscReferenceLabel);
      const inputRoleLabel = document.createElement('label');
      inputRoleLabel.textContent = 'Reference input for this RF channel';
      const topology = processingControlCapabilities().topology;
      const inputCount = topology.channelCount;
      const inherited = topology.referenceChannel;
      const roleOptions = [
        {value: 'inherit', label: topology.referenceMode === 'array_eigenbeam' ?
          'Use array-eigenbeam inputs' : `Use input default (Ch ${inherited})`}];
      for (let channel = 0; channel < inputCount; channel += 1)
        roleOptions.push({value: String(channel), label: `Ch ${channel}`});
      const role = selectInput(roleOptions,
        Number.isInteger(item.reference_channel) ? String(item.reference_channel) : 'inherit',
        selected => {
          if (role.selectedOptions[0]?.disabled) return;
          if (selected === 'inherit') {
            item.input_role_mode = 'manual';
            delete item.reference_channel; delete item.surveillance_channels;
          } else {
            item.input_role_mode = 'manual';
            item.reference_channel = Number(selected);
            item.surveillance_channels = Array.from({length: inputCount}, (_, channel) => channel)
              .filter(channel => channel !== item.reference_channel);
          }
          commit();
        });
      role.title = 'Use the receiver topology or explicitly choose this illuminator’s reference input.';
      const coherent = ['coherent_beam_bank', 'noise_whitened_beam_bank']
        .includes(activeConfig.process.surveillance_fusion?.mode);
      if (coherent) {
        [...role.options].filter(option => option.value !== 'inherit').forEach(option => { option.disabled = true; });
        role.title = 'Coherent lanes must inherit matching array-eigenbeam reference and surveillance inputs. Saved overrides remain visible.';
      }
      inputRoleLabel.appendChild(role); card.appendChild(inputRoleLabel);
      const originalFrequency = item.frequency_hz; item.frequency_hz = originalFrequency / 1e6;
      addNumber('Frequency (MHz)', 'frequency_hz', 1e6); item.frequency_hz = originalFrequency;
      const originalBandwidth = item.occupied_bandwidth_hz;
      item.occupied_bandwidth_hz = originalBandwidth / 1000;
      addNumber('Bandwidth (kHz)', 'occupied_bandwidth_hz', 1000);
      item.occupied_bandwidth_hz = originalBandwidth;
      const originalRate = item.output_sample_rate_hz;
      item.output_sample_rate_hz = originalRate / 1000;
      addNumber('Output rate (kS/s)', 'output_sample_rate_hz', 1000);
      item.output_sample_rate_hz = originalRate;
      addNumber('Filter taps', 'filter_taps_per_phase');
      addText('Transmitter', 'transmitter.name');
      addText('Transmitter site group', 'transmitter.site_id');
      addNumber('Latitude', 'transmitter.latitude');
      addNumber('Longitude', 'transmitter.longitude');
      addNumber('Altitude (m)', 'transmitter.altitude');
      const siteHelp = document.createElement('p');
      siteHelp.className = 'config-field-help illuminator-calibration-note';
      siteHelp.textContent = 'Coherent processing uses this lane’s explicit site, RF frequency, and output rate. Global transmitter geometry is never inherited; calibration files must match all three.';
      card.appendChild(siteHelp);
      for (const [label, key, min, max] of [
        ['Coherent site latitude', 'latitude', -90, 90],
        ['Coherent site longitude', 'longitude', -180, 180],
        ['Coherent site altitude (m)', 'altitude', undefined, undefined]]) {
        const input = addText(label, `transmitter_location.${key}`, Number, true);
        input.type = 'number'; input.step = 'any';
        if (min !== undefined) input.min = String(min);
        if (max !== undefined) input.max = String(max);
        input.placeholder = 'Required for coherent lanes';
      }
      for (const [label, key] of [['Lane detection calibration file', 'detection_manifold_file'],
        ['Lane bearing calibration file', 'bearing_manifold_file']]) {
        const input = addText(label, key, value => value.trim(), true);
        input.placeholder = 'Optional lane-specific calibration';
        input.title = 'An inherited or lane-specific calibration file must match this lane’s RF frequency, sample rate, and surveyed site.';
      }
      const remove = document.createElement('button');
      remove.type = 'button'; remove.className = 'button-secondary illuminator-remove'; remove.textContent = 'Remove';
      remove.addEventListener('click', () => {
        items.splice(index, 1); commit(); renderEditor();
      });
      card.appendChild(remove); wrapper.appendChild(card);
    });
    const add = document.createElement('button');
    add.type = 'button'; add.className = 'button-secondary'; add.textContent = 'Add illuminator';
    add.addEventListener('click', () => {
      items.push({id: `illuminator-${items.length + 1}`, enabled: true, standard: 'FM',
        frequency_hz: activeConfig.capture?.fc || 100000000,
        occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
        filter_taps_per_phase: 24, atsc_reference_mode: 'raw', input_role_mode: 'manual',
        transmitter: {name: '', latitude: 0, longitude: 0, altitude: 0}});
      commit(); renderEditor();
    });
    wrapper.appendChild(add);
  };
  render();
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
    wrapper.title = 'Newest healthy position wins.';

    const lagCpis = Number(activeConfig.truth?.adsb?.association_lag_cpis || 0);
    const cpiSeconds = Number(activeConfig.process?.data?.cpi);
    const alignment = document.createElement('p');
    alignment.className = 'adsb-source-note';
    alignment.textContent = Number.isFinite(cpiSeconds) ?
      `Presentation hold: ${lagCpis} CPIs (${(lagCpis * cpiSeconds).toFixed(3)} s).` :
      `Presentation hold: ${lagCpis} CPIs.`;
    wrapper.appendChild(alignment);

    const localHeading = document.createElement('strong');
    localHeading.textContent = 'Local receivers';
    wrapper.appendChild(localHeading);
    const diagnostics = document.createElement('div');
    diagnostics.className = 'adsb-source-diagnostics';
    diagnostics.setAttribute('role', 'status');
    diagnostics.textContent = 'Measuring source publication cadence…';
    wrapper.appendChild(diagnostics);
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
    const exchangeTimingNote = document.createElement('p');
    exchangeTimingNote.className = 'adsb-source-note warning';
    exchangeTimingNote.textContent = 'Stock readsb 1 s JSON is too slow for CPI pass scoring. Use --write-json-every 0.25.';
    wrapper.appendChild(exchangeTimingNote);
    const hasSite = ADSB_PUBLIC_EXCHANGES.every(exchange => adsbPublicSource(exchange.id));
    for (const exchange of ADSB_PUBLIC_EXCHANGES) {
      const row = document.createElement('div');
      row.className = 'adsb-exchange-row';
      const copy = document.createElement('span');
      const title = document.createElement('b');
      title.textContent = exchange.label;
      const detail = document.createElement('small');
      detail.textContent = exchange.description;
      detail.hidden = true;
      row.title = exchange.description;
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
    Promise.resolve().then(async () => {
      try {
        const response = await fetchStatusResource(liveApiUrl('/api/adsb/status'),
          {cache: 'no-store'}, 5000);
        if (!response.ok || !diagnostics.isConnected) return;
        const status = await response.json();
        const local = (status.sources || []).filter(source =>
          !String(source.id || '').startsWith('exchange:'));
        diagnostics.replaceChildren();
        for (const source of local) {
          const row = document.createElement('p');
          const cadence = source.publication_cadence;
          const interval = Number(cadence?.median_interval_s);
          row.className = `adsb-cadence ${cadence?.statistics_suitable ? 'good' : 'warning'}`;
          row.textContent = `${source.label || source.id}: ${Number.isFinite(interval) ?
            `${interval.toFixed(2)} s updates (${(1 / interval).toFixed(1)} Hz)` : 'measuring cadence'}${
            cadence?.statistics_suitable === false ? ' — too slow for reliable CPI-level pass statistics' : ''}`;
          diagnostics.appendChild(row);
        }
        if (!local.length) diagnostics.textContent = 'No local ADS-B receiver is configured.';
      } catch (_) { if (diagnostics.isConnected) diagnostics.textContent = 'Source cadence is temporarily unavailable.'; }
    });

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
    photoDetail.hidden = true;
    photoDetail.id = 'adsb-photo-help';
    photoCopy.append(photoTitle, photoDetail);
    const photoSelect = document.createElement('select');
    photoSelect.dataset.adsbPhotoProvider = 'true';
    photoSelect.setAttribute('aria-label', 'Aircraft photo provider');
    photoSelect.setAttribute('aria-describedby', photoDetail.id);
    photoSelect.title = 'Cached aircraft photos';
    for (const [value, label] of [['disabled', 'Off'],
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
  if (key === 'capture.device.heimdall.gain_control') {
    const wrapper = document.createElement('div'); wrapper.className = 'config-gain-policy';
    const manual = document.createElement('div'); manual.className = 'config-gain-manual';
    const gains = Array.isArray(activeConfig.capture.device.heimdall.channel_gains) ?
      activeConfig.capture.device.heimdall.channel_gains :
      Array.from({length: activeConfig.capture.device.channel_count || 1}, () =>
        typeof activeConfig.capture.device.heimdall.gain === 'number' ? activeConfig.capture.device.heimdall.gain : 0);
    manual.appendChild(arrayInput(gains, ['capture', 'device', 'heimdall', 'channel_gains']));
    const modeValue = value === 'fixed' ? 'fixed' : 'measured_per_channel';
    const mode = selectInput([{value: 'fixed', label: `Manual${
        Array.isArray(activeConfig.capture.device.heimdall.channel_gains) ?
          ` (${activeConfig.capture.device.heimdall.channel_gains.map(gain =>
            Number(gain).toFixed(1)).join(', ')} dB)` : ''}`},
      {value: 'measured_per_channel', label: `Automatic${
        Array.isArray(activeConfig.capture.device.heimdall.channel_gains) ?
          ` (${activeConfig.capture.device.heimdall.channel_gains.map(gain =>
            Number(gain).toFixed(1)).join(', ')} dB selected)` : ''}`}], modeValue,
    selected => {
      applyComposite(config => configProfiles.applyGainMode(config,
        selected === 'fixed' ? 'manual' : 'automatic'));
      manual.hidden = selected !== 'fixed';
    });
    manual.hidden = modeValue !== 'fixed';
    wrapper.append(mode, manual); return wrapper;
  }
  if (key === 'capture.device.lnaState') return rspDuoLnaInput(value);
  if (key === 'process.performance.acceleration')
    return selectInput([{value: 'auto', label: 'Automatic'}, {value: 'cpu', label: 'CPU only'}],
      value === 'cpu' ? 'cpu' : 'auto', selected => applyComposite(config =>
        configProfiles.applyProcessingMode(config, selected === 'cpu' ? 'cpu' : 'automatic')));
  if (key === 'process.tracker.profile')
    return selectInput([{value: 'balanced', label: 'Balanced (recommended)'},
      {value: 'conservative', label: 'Conservative'},
      {value: 'permissive', label: 'Permissive'}], value,
      selected => applyComposite(config => configProfiles.applyTrackingProfile(config, selected)));
  if (Array.isArray(value)) return arrayInput(value, path);
  const selection = rule.control;
  if (selection?.choices) return declaredSelect(value, path, rule, selection);
  if (rule.choices)
    return selectInput(rule.choices, value, selected =>
      setValue(path, rule.type === 'number' ? Number(selected) : selected));
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
    const presentation = rule.control || (key === 'process.data.overlap' ? {scale: 1, unit: 's'} : null);
    const scale = presentation?.scale || 1;
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
      unit.textContent = presentation.unit;
      wrapper.append(input, unit);
      return wrapper;
    }
  } else {
    input.type = 'text';
    const optionalRspSerial = key === 'capture.device.serial' &&
      activeConfig.capture?.device?.type === 'RspDuo';
    const conditionalExperimentText = key === 'save.complex_cutout.path';
    const optionalReplayPin = /^capture\.replay\.(recording|acquisition)_sha256$/.test(key);
    // Empty experiment selectors/paths are valid while disabled. The API
    // enforces the exact lane/path requirements when the experiment is enabled.
    input.required = !optionalRspSerial && !conditionalExperimentText && !optionalReplayPin;
    if (optionalRspSerial) input.maxLength = 160;
    if (optionalReplayPin) { input.maxLength = 64; input.pattern = '[a-f0-9]{64}'; }
    input.value = value ?? '';
    input.addEventListener('input', () => setValue(path, optionalReplayPin && input.value === '' ? undefined : input.value));
  }
  return input;
}

function field(value, path) {
  const [labelText, description] = metadata(path);
  const key = path.join('.');
  const control = key === 'save.path' ? recordingFolderControl(value, path) : typedInput(value, path);
  const rule = capabilities.fieldRules?.[key];
  const unusedDedicatedInput = key === 'capture.device.reference_channel' &&
    activeConfig.process?.reference_synthesis?.mode === 'array_eigenbeam';
  const readOnly = !rule || rule.readOnly || !capabilities.editable || unusedDedicatedInput;
  const help = !readOnly ? description : unusedDedicatedInput ?
    'Not used in combined-input mode. The reference is synthesized from the inputs below.' :
    `${description} ${rule?.readOnly || (!rule ? 'Unknown setting preserved; edit in YAML if needed.' : '')}`;
  const wrapper = compositeField(labelText, help, control, key, {readOnly});
  if (control.classList?.contains('illuminator-list'))
    wrapper.classList.add('config-field-wide', 'config-field-illuminators');
  if (control.classList?.contains('adsb-source-editor'))
    wrapper.classList.add('config-field-wide', 'config-field-adsb-sources');
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
    if (!capabilities.editable) return;
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
    finally { create.disabled = !capabilities.editable; }
  });
  wrapper.append(select, create); void load(value);
  return wrapper;
}

function inlineSettingsGroup(details, summary, content, fieldCount) {
  const section = document.createElement('section');
  for (const [key, value] of Object.entries(details.dataset)) section.dataset[key] = value;
  section.className = `${details.className} config-group-inline${fieldCount === 1 ? ' config-group-single' : ''}`;
  if (fieldCount > 1) {
    const heading = document.createElement('div');
    heading.className = 'config-group-heading';
    while (summary.firstChild) heading.appendChild(summary.firstChild);
    section.appendChild(heading);
  }
  section.appendChild(content);
  return section;
}

function finishSettingsGroup(details, summary, content, topLevel) {
  const fieldCount = content.querySelectorAll('.config-field').length;
  if (topLevel || fieldCount >= 3) {
    details.append(summary, content);
    return details;
  }
  return inlineSettingsGroup(details, summary, content, fieldCount);
}

function revealSmallSettingsGroups(root) {
  const groups = [...root.querySelectorAll('details.config-group')].reverse();
  for (const details of groups) {
    if (details.classList.contains('config-group-root')) continue;
    const fieldCount = details.querySelectorAll('.config-field').length;
    if (fieldCount < 1 || fieldCount >= 3) continue;
    const summary = details.querySelector(':scope > summary');
    if (!summary) continue;
    const content = details.querySelector(':scope > .config-fields');
    if (content) details.replaceWith(
      inlineSettingsGroup(details, summary, content, fieldCount));
  }
}

function moveArrayEditorToArrayTab(panels, enabled) {
  if (!enabled) return false;
  const geometry = panels.querySelector('#settings-panel-capture .kraken-geometry');
  const arrayPanel = panels.querySelector('#settings-panel-array');
  if (!geometry || !arrayPanel) return false;
  geometry.open = true;
  geometry.classList.add('kraken-geometry-standalone');
  arrayPanel.appendChild(geometry);
  return true;
}

function group(key, value, path, topLevel = false, options = {}) {
  const currentPath = [...path, key];
  const dotted = currentPath.join('.');
  const kraken = activeConfig.capture?.device?.type === 'Kraken';
  const processing = processingControlCapabilities();
  let [title, description] = metadata(currentPath);
  if (options.groupMeta?.[dotted]) [title, description] = options.groupMeta[dotted];
  if (dotted === 'capture.device') {
    const profile = capabilities.deviceProfiles?.find(item => item.type === value.type);
    title = profile ? profile.label : value.type || title;
    description = kraken ? 'Tuning and receiver controls' : 'Connection and controls';
  }
  if (kraken && dotted === 'capture.device.heimdall') {
    title = 'Radio connection & gain';
    description = 'Suite connection and gain';
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
  small.hidden = true;
  summary.title = description;
  heading.append(strong, small);
  summary.append(heading);
  const content = document.createElement('div');
  content.className = 'config-fields';
  if (dotted === 'capture' && value.device?.type !== undefined && !processing.recorded)
    content.appendChild(field(value.device.type, ['capture', 'device', 'type']));
  const order = {
    capture: ['fc', 'fs', 'device', 'aircraft_clips', 'replay'],
    'capture.device': ['heimdall', 'channel_count', 'surveillance_channels'],
    'capture.device.heimdall': ['gain_control', 'gain', 'channel_gains', 'host', 'port', 'control_port'],
    process: ['data', 'ambiguity', 'clutter', 'detection', 'tracker', 'performance'],
    'process.reference_synthesis': ['mode', 'channels']
  }[dotted] || [];
  const entries = Object.entries(dotted === 'process' ?
    {...value, performance: value.performance || {}} : dotted === 'capture.replay' ?
      {recording_sha256: '', acquisition_sha256: '', ...value} : value).sort(([left], [right]) =>
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
    if (dotted === 'process' && ['clutter', 'detection', 'tracker', 'performance', 'delay_reference_sample_rate_hz'].includes(childKey)) return;
    if (dotted === 'capture.device' && childKey === 'type') return;
    if (dotted === 'capture.device' && childKey === 'array_geometry') return;
    if (processing.recorded && dotted === 'capture.device') return;
    if (processing.recorded && dotted === 'capture' && ['fc', 'fs'].includes(childKey)) return;
    if (kraken && dotted === 'capture.device' && ['channel_count', 'reference_channel',
      'surveillance_channels'].includes(childKey)) return;
    if (kraken && dotted === 'capture.device.heimdall' && ['gain', 'channel_gains'].includes(childKey)) return;
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
  if (advancedFields.children.length)
    content.appendChild(advancedReference);
  const savedGeometry = activeConfig.capture?.device?.array_geometry;
  if (dotted === 'capture.device' && geometryEditor &&
      (processing.arrayEditor || savedGeometry !== undefined))
    content.appendChild(makeGeometryEditor());
  if (dotted === 'process') appendCompositeRadarSettings(content);
  return finishSettingsGroup(details, summary, content, topLevel);
}

function renderReceiverFeatures() {
  const section = document.createElement('section');
  section.className = 'receiver-feature-section';
  const topology = processingControlCapabilities().topology;
  const featureOptions = {groupMeta: {
    'process.detection': ['Coherent integration', 'Combine consecutive radar intervals and search short motion paths.'],
    'process.tracker': ['Persistent tracker recovery', 'Bounded motion-consistent recovery settings available to every supported SDR path.']
  }};
  const process = activeConfig.process || {};
  const tracker = process.tracker || {};
  const residentTracker = {};
  for (const key of ['gpu_capacity'])
    if (tracker[key] !== undefined && !experimentalSettingPath(`process.tracker.${key}`))
      residentTracker[key] = tracker[key];
  if (topology.coherentSurveillance && tracker.beam_tolerance !== undefined &&
      !experimentalSettingPath('process.tracker.beam_tolerance'))
    residentTracker.beam_tolerance = tracker.beam_tolerance;
  if (tracker.initiate?.stationary_clutter !== undefined)
    residentTracker.initiate = {stationary_clutter: tracker.initiate.stationary_clutter};
  if (Object.keys(residentTracker).length)
    section.appendChild(group('tracker', residentTracker, ['process'], false, featureOptions));
  return section;
}

function renderExperimentalSettings() {
  const section = document.createElement('section');
  section.className = 'experimental-settings';
  const process = activeConfig.process || {};
  const coherentArray = processingControlCapabilities().topology.coherentSurveillance;
  const add = (key, value, path) => {
    if (value !== undefined) section.appendChild(group(key, value, path, false));
  };
  add('remote_id', activeConfig.truth?.remote_id, ['truth']);
  add('multi_illuminator', process.multi_illuminator, ['process']);
  add('doppler_ridge_clutter', process.tracker?.initiate?.doppler_ridge_clutter,
    ['process', 'tracker', 'initiate']);
  add('complex_cutout', activeConfig.save?.complex_cutout, ['save']);
  if (process.reference_synthesis) {
    const protection = {target_protection_enabled: process.reference_synthesis.target_protection_enabled ?? false,
      target_protection_beams: process.reference_synthesis.target_protection_beams ?? 12,
      target_protection_beam_index: process.reference_synthesis.target_protection_beam_index ?? 0};
    section.appendChild(group('reference_synthesis', protection, ['process'], false,
      {groupMeta: {'process.reference_synthesis': ['Experimental target-protected reference',
        'Default off. Single coherent CPU/portable GPU replay only; one predeclared 4–360 direction bank member. No truth features, extra worker, or automatic search. Projects one reference vector with an explicit 5% retention floor; degeneracy fails closed. Timing reports the selected index; reference weights and complex CAF export provide results. False-track and production-route qualification are pending.']}}));
  } else {
    const note = document.createElement('p'); note.className = 'config-capability-note';
    note.textContent = 'Target-protected reference is unavailable without coherent array reference synthesis; qualified coherent replay may provide that topology.';
    section.appendChild(note);
  }
  add('manifold_recorder', process.bearing?.manifold_recorder, ['process', 'bearing']);
  const diagnosticKeys = ['map', 'detection']
    .filter(key => activeConfig.save?.[key] !== undefined);
  if (diagnosticKeys.length) {
    const diagnostics = document.createElement('details');
    diagnostics.className = 'config-group';
    diagnostics.dataset.configGroup = 'save.diagnostic_exports';
    const summary = document.createElement('summary');
    const heading = document.createElement('span');
    const strong = document.createElement('strong');
    const small = document.createElement('small');
    strong.textContent = 'Diagnostic exports (not IQ recording)';
    small.textContent = 'Legacy processed products for offline debugging. These do not record receiver samples or aircraft clips.';
    small.hidden = true;
    summary.title = small.textContent;
    heading.append(strong, small);
    summary.append(heading);
    const fields = document.createElement('div');
    fields.className = 'config-fields';
    diagnosticKeys.forEach(key => fields.appendChild(field(activeConfig.save[key], ['save', key])));
    section.appendChild(finishSettingsGroup(diagnostics, summary, fields, false));
  }
  if (!section.querySelector('.config-field')) {
    const empty = document.createElement('p');
    empty.className = 'config-capability-note';
    empty.textContent = 'This build does not advertise any experimental controls.';
    section.appendChild(empty);
  }
  return section;
}

function renderRecordingSettings() {
  const section = document.createElement('section');
  section.className = 'recording-settings';
  if (activeConfig.capture?.aircraft_clips &&
      typeof activeConfig.capture.aircraft_clips === 'object') {
    section.appendChild(group('aircraft_clips', activeConfig.capture.aircraft_clips,
      ['capture'], true));
  } else {
    const unavailable = document.createElement('p');
    unavailable.className = 'config-capability-note';
    unavailable.textContent = 'Automatic aircraft clips require a backend version that advertises those settings.';
    section.appendChild(unavailable);
  }
  if (activeConfig.save?.path !== undefined)
    section.appendChild(field(activeConfig.save.path, ['save', 'path']));
  if (activeConfig.save?.retention && typeof activeConfig.save.retention === 'object')
    section.appendChild(group('retention', activeConfig.save.retention, ['save']));
  return section;
}

function makeGeometryEditor() {
  const available = processingControlCapabilities();
  if (receiverInvalidatedGeometry)
    geometryEditor?.invalidate(activeConfig.capture?.device?.array_geometry);
  const editor = geometryEditor.render(document, activeConfig, rebuild => {
    if (rebuild) refreshGeometryEditor();
    scheduleValidation();
  }, () => Boolean(capabilities.editable && !configurationBusy()),
  (_count, previousCount) => normalizeKrakenChannels(activeConfig, previousCount), available.topology);
  const content = editor.querySelector(':scope > .config-fields');
  if (!content || !available.arrayEditor) return editor;
  const process = activeConfig.process || {};
  if (process.reference_synthesis) {
    const tuning = Object.fromEntries(['analysis_samples', 'analysis_interval', 'power_iterations',
      'covariance_smoothing', 'diagonal_loading'].filter(key =>
      process.reference_synthesis[key] !== undefined).map(key => [key, process.reference_synthesis[key]]));
    if (Object.keys(tuning).length)
      content.appendChild(group('reference_synthesis', tuning, ['process'], false,
        {groupMeta: {'process.reference_synthesis': ['Reference estimation tuning',
          'Update cadence and numerical stability for the array-derived reference.']}}));
  }
  if (process.surveillance_fusion)
    content.appendChild(group('surveillance_fusion', process.surveillance_fusion, ['process'], false,
      {groupMeta: {'process.surveillance_fusion': ['Array combining',
        'Combine surveillance antennas with power or calibrated coherent beams.']},
      skipPath: experimentalSettingPath}));
  if (process.bearing)
    content.appendChild(group('bearing', process.bearing, ['process'], false,
      {groupMeta: {'process.bearing': ['Bearing',
        'Estimate target direction from the configured and surveyed array.']},
      skipPath: experimentalSettingPath}));
  return editor;
}

function refreshGeometryEditor() {
  const old = document.querySelector('.kraken-geometry');
  if (old && geometryEditor) {
    if (processingControlCapabilities().arrayEditor || activeConfig.capture?.device?.array_geometry !== undefined) {
      const next = makeGeometryEditor();
      // Technical layout moves fields outside the geometry section. Refresh
      // their existing controls rather than leaving stale duplicate inputs.
      const moved = [...document.querySelectorAll('.config-field[data-path]')]
        .filter(field => !old.contains(field));
      for (const field of [...next.querySelectorAll('.config-field[data-path]')]) {
        const existing = moved.find(item => item.dataset.path === field.dataset.path);
        if (existing) existing.replaceWith(field);
      }
      old.replaceWith(next);
    }
    else old.remove();
  }
}

function updateDirtyState() {
  const dirty = serializeConfig(activeConfig) !== originalConfig;
  const editedPaths = new Set([...document.querySelectorAll('.config-field[data-path]')]
    .map(item => item.dataset.path).filter(path => path && settingEdited(path)));
  const editedCount = dirty ? Math.max(1, editedPaths.size) : 0;
  document.querySelector('.config-toolbar')?.classList.toggle('has-changes',
    dirty || receiverApplyPending || restartRetry || capabilities.setupRequired);
  const action = document.getElementById('config-save');
  const later = document.getElementById('config-save-later');
  const reset = document.getElementById('config-reset');
  if (action) {
    action.disabled = configurationBusy() || (!dirty && !restartRetry && !receiverApplyPending && !capabilities.setupRequired) ||
      pendingLiveConfig !== null || !capabilities.editable || validationPending || validationErrors.length > 0;
    if (!configurationBusy()) action.textContent = replayLaunchPending ? 'Start replay' :
      restartRetry ? 'Retry restart' :
      receiverApplyPending ? (capabilities.restartAvailable ? 'Apply & Restart' : 'Apply (manual restart)') :
      capabilities.restartAvailable ? 'Save & Restart' : 'Save (manual restart)';
  }
  if (later) later.disabled = configurationBusy() || (!dirty && !capabilities.setupRequired) ||
    pendingLiveConfig !== null || !capabilities.editable || validationPending || validationErrors.length > 0;
  if (reset) {
    reset.disabled = configurationBusy() || (!dirty && pendingLiveConfig === null);
    reset.textContent = pendingLiveConfig === null ? 'Discard changes' : 'Reload saved config';
  }
  const state = document.getElementById('config-state');
  if (state) state.textContent = pendingLiveConfig !== null ?
    'Saved settings changed' : dirty ? `${editedCount} unsaved change${editedCount === 1 ? '' : 's'}` :
      receiverApplyPending ? 'Apply settings to confirm' : '';
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
    validationErrors = [...new Set(nativeErrors.concat(
      configProfiles.temporalConfirmationErrors(activeConfig), result.valid ? [] :
      (result.errors || ['The configuration is invalid.'])))];
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
  updateProcessingControlAvailability();
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
    const measuredGains = Array.isArray(status.actual.adcHeadroom?.channels) ?
      status.actual.adcHeadroom.channels.map(channel => channel.actualGainDb).
        filter(Number.isFinite) : [];
    const gainText = measuredGains.length ?
      `Automatic (${measuredGains.map(gain => gain.toFixed(1)).join(', ')} dB)` :
      Number.isFinite(status.actual.gain) ?
        status.actual.gain < 0 ? 'Automatic' : `${status.actual.gain.toFixed(1)} dB` :
        'Unavailable';
    const rows = [
      ['Frequency', formatHertz(status.expected.centerFrequency),
        formatHertz(status.actual.centerFrequency)],
      ['Sample rate', formatHertz(status.expected.sampleRate, 'MS/s'),
        formatHertz(status.actual.sampleRate, 'MS/s')],
      ['Channels', String(status.expected.channels),
        status.actual.channels === null ? 'Unavailable' : String(status.actual.channels)],
      ['Mode', status.expected.mode, status.actual.mode || 'Unavailable'],
      ['Gain', 'Set in Heimdall', gainText]
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

function receiverUsesUpstreamStatus(receiverType) {
  // Kraken is managed by the separate Heimdall receiver. Direct receivers
  // report their own device/API readback in the startup card instead.
  return receiverType === 'Kraken';
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
        processingTopology.processingCapabilities(activeConfig).liveMchq &&
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

function valueAtPath(value, dotted) {
  return dotted.split('.').reduce((item, key) => item?.[key], value);
}

function rememberOriginalConfig(config) {
  originalConfig = serializeConfig(config);
  originalConfigObject = JSON.parse(JSON.stringify(config));
}

function settingEdited(dotted) {
  if (!originalConfigObject) return false;
  return serializeConfig(valueAtPath(activeConfig, dotted)) !==
    serializeConfig(valueAtPath(originalConfigObject, dotted));
}

function makeSettingsFilter(tabs, panels, activate) {
  const toolbar = document.createElement('div');
  toolbar.className = 'settings-filter';
  const search = document.createElement('label');
  search.className = 'settings-search';
  const searchLabel = document.createElement('span');
  searchLabel.textContent = 'Find a setting';
  const input = document.createElement('input');
  input.type = 'search';
  input.placeholder = 'Gain, tracking, port…';
  input.value = settingsSearch;
  input.autocomplete = 'off';
  const edited = document.createElement('label');
  edited.className = 'settings-edited';
  const checkbox = document.createElement('input');
  checkbox.type = 'checkbox';
  checkbox.checked = settingsEditedOnly;
  edited.append(checkbox, document.createTextNode('Edited only'));
  const status = document.createElement('span');
  status.className = 'settings-filter-status';
  status.setAttribute('role', 'status');
  search.append(searchLabel, input);
  toolbar.append(search, edited, status);
  let restoreState = null;
  let lastRevealedField = null;

  const apply = () => {
    const query = settingsSearch.trim().toLowerCase();
    const filtering = Boolean(query || settingsEditedOnly);
    const fields = [...panels.querySelectorAll('.config-field')];
    if (filtering && !restoreState) {
      restoreState = {
        tab: tabs.querySelector('[role="tab"][aria-selected="true"]')?.dataset.tab,
        disclosures: new Map([...panels.querySelectorAll('details')]
          .map(details => [details, details.open]))
      };
    }
    let visible = 0;
    for (const field of fields) {
      const path = field.dataset.path || '';
      const matchesText = !query || `${path} ${field.textContent}`.toLowerCase().includes(query);
      const matchesEdit = !settingsEditedOnly || settingEdited(path);
      field.hidden = !(matchesText && matchesEdit);
      field.classList.toggle('config-field-edited', settingEdited(path));
      field.classList.toggle('settings-search-match', Boolean(query && matchesText && matchesEdit));
      if (!field.hidden) visible += 1;
    }
    const groups = [...panels.querySelectorAll('[data-config-group]')].reverse();
    for (const group of groups) {
      const descendants = [...group.querySelectorAll('.config-field')];
      if (descendants.length) group.hidden = filtering && !descendants.some(field => !field.hidden);
    }
    const availableTabs = [];
    for (const button of tabs.querySelectorAll('[role="tab"]')) {
      const panel = panels.querySelector(`[data-tab="${button.dataset.tab}"]`);
      const descendants = [...panel.querySelectorAll('.config-field')];
      const customTextMatch = query && panel.textContent.toLowerCase().includes(query);
      const matches = !filtering || descendants.some(field => !field.hidden) ||
        (!settingsEditedOnly && customTextMatch);
      button.hidden = !matches;
      if (matches) availableTabs.push(button.dataset.tab);
    }
    const selected = tabs.querySelector('[role="tab"][aria-selected="true"]');
    const selectedPanel = selected ? panels.querySelector(`[data-tab="${selected.dataset.tab}"]`) : null;
    const firstVisibleField = fields.find(field => !field.hidden);
    const matchedPanel = firstVisibleField?.closest('[role="tabpanel"]');
    if (selected?.hidden && availableTabs.length) activate(availableTabs[0], false);
    else if (query && matchedPanel && !selectedPanel?.contains(firstVisibleField))
      activate(matchedPanel.dataset.tab, false);
    if (filtering) {
      for (const field of fields.filter(item => !item.hidden)) {
        let ancestor = field.parentElement;
        while (ancestor && ancestor !== panels) {
          if (ancestor.tagName === 'DETAILS') ancestor.open = true;
          ancestor = ancestor.parentElement;
        }
      }
    } else if (restoreState) {
      for (const [details, open] of restoreState.disclosures)
        if (details.isConnected) details.open = open;
      if (restoreState.tab && tabs.querySelector(`[data-tab="${restoreState.tab}"]`))
        activate(restoreState.tab, false);
      restoreState = null;
      lastRevealedField = null;
    }
    if (query) {
      if (firstVisibleField && firstVisibleField !== lastRevealedField) {
        firstVisibleField.scrollIntoView?.({block: 'center', inline: 'nearest'});
        lastRevealedField = firstVisibleField;
      }
    }
    status.textContent = filtering ? `${visible} matching setting${visible === 1 ? '' : 's'}` : '';
  };
  input.addEventListener('input', () => { settingsSearch = input.value; apply(); });
  checkbox.addEventListener('change', () => {
    settingsEditedOnly = checkbox.checked;
    apply();
  });
  refreshSettingsFilter = apply;
  window.setTimeout(apply, 0);
  return toolbar;
}

function invalidateGeometryAfterReceiverChange() {
  receiverInvalidatedGeometry = true;
  geometryEditor?.invalidate(activeConfig.capture?.device?.array_geometry);
  refreshGeometryEditor();
}

function primarySettingPath(tab, path) {
  if (!path) return true;
  if (tab === 'array') return path.startsWith('capture.device.array_geometry') ||
    ['capture.device.channel_count', 'capture.device.reference_channel',
      'capture.device.surveillance_channels', 'process.reference_synthesis.mode',
      'process.reference_synthesis.channels', 'process.surveillance_fusion.mode',
      'process.bearing.enable', 'process.bearing.method'].includes(path);
  if (tab === 'capture') return path.startsWith('location.') ||
    PRIMARY_SETTING_PATHS.has(path);
  if (tab === 'process') return PRIMARY_SETTING_PATHS.has(path);
  if (tab === 'truth') return ['truth.adsb.enabled', 'truth.adsb.tar1090',
    'truth.adsb.aircraft_photo_provider'].includes(path);
  if (tab === 'save') return path === 'save.path' ||
    path.startsWith('capture.aircraft_clips.') || path.startsWith('save.retention.');
  if (tab === 'network') return path === 'network.ip' ||
    path.startsWith('network.tracker_forward.');
  return tab === 'experimental';
}

function removeEmptySettingsGroups(panel, technical) {
  [...panel.querySelectorAll('.config-group')].reverse().forEach(group => {
    if (group === technical || group.classList.contains('kraken-geometry')) return;
    if (group.querySelector('.config-field,input,select,button')) return;
    group.remove();
  });
}

function organizeTechnicalControls(panel, tab) {
  const fields = [...panel.querySelectorAll('.config-field')];
  const technicalFields = fields.filter(item => item.closest('.composite-tuning') ||
    !primarySettingPath(tab, item.dataset.path || ''));
  if (!technicalFields.length) return;
  const details = document.createElement('details');
  details.className = 'config-group settings-technical';
  details.dataset.configGroup = `${tab}.technical`;
  const summary = document.createElement('summary');
  const heading = document.createElement('span');
  const title = document.createElement('strong');
  const description = document.createElement('small');
  title.textContent = 'Technical controls';
  description.textContent = 'Ports and expert tuning';
  description.hidden = true;
  summary.title = description.textContent;
  heading.append(title, description);
  summary.append(heading);
  const body = document.createElement('div');
  body.className = 'config-fields';
  technicalFields.forEach(field => body.appendChild(field));
  details.append(summary, body);
  removeEmptySettingsGroups(panel, details);
  panel.appendChild(details);
  revealSmallSettingsGroups(panel);
  panel.querySelectorAll('.receiver-feature-section').forEach(section => {
    if (!section.querySelector('.config-field,input,select,button')) section.remove();
  });
}

function renderEditor() {
  const target = document.getElementById('config-fields');
  let upstream = document.getElementById('upstream-status');
  if (upstream) upstream.remove();
  target.replaceChildren();
  const tabs = document.createElement('div');
  tabs.className = 'config-tabs';
  tabs.setAttribute('role', 'tablist');
  tabs.setAttribute('aria-label', 'Settings categories');
  const panels = document.createElement('div');
  panels.className = 'config-tab-panels';
  const receiverType = activeConfig.capture?.device?.type;
  if (!upstream && receiverUsesUpstreamStatus(receiverType)) {
    upstream = document.createElement('section');
    upstream.id = 'upstream-status';
    upstream.className = 'upstream-status';
    upstream.innerHTML = '<div class="upstream-header"><strong>Receiver status</strong><span class="upstream-badge">Checking…</span></div>';
  }
  const {arrayEditor:hasArrayTab, recorded} = processingControlCapabilities();
  const tabOrder = ['capture', 'array', 'location', 'process', 'truth', 'save', 'experimental', 'network'];
  const entries = Object.entries(activeConfig).filter(([key]) => key !== 'location');
  if (hasArrayTab) entries.push(['array', {}]);
  entries.sort(([left], [right]) =>
    (tabOrder.includes(left) ? tabOrder.indexOf(left) : 100) -
    (tabOrder.includes(right) ? tabOrder.indexOf(right) : 100));
  entries.push(['experimental', {}]);
  entries.sort(([left], [right]) =>
    (tabOrder.includes(left) ? tabOrder.indexOf(left) : 100) -
    (tabOrder.includes(right) ? tabOrder.indexOf(right) : 100));
  let savedTab = null;
  try { savedTab = sessionStorage.getItem('blah2-settings-tab'); }
  catch (_) { /* Remembering a tab is optional. */ }
  const requestedTabRaw = new URLSearchParams(window.location.search).get('tab');
  const requestedTab = requestedTabRaw === 'location' ? 'capture' : requestedTabRaw;
  if (savedTab === 'location') savedTab = 'capture';
  const initialTab = capabilities.setupRequired ? 'capture' :
    entries.some(([key]) => key === requestedTab) ? requestedTab :
      entries.some(([key]) => key === savedTab) ? savedTab : entries[0]?.[0];
  const activate = (selectedKey, remember = true) => {
    tabs.querySelectorAll('[role="tab"]').forEach(button => {
      const selected = button.dataset.tab === selectedKey;
      button.classList.toggle('active', selected);
      button.setAttribute('aria-selected', String(selected));
      button.tabIndex = selected ? 0 : -1;
    });
    panels.querySelectorAll('[role="tabpanel"]').forEach(panel => {
      panel.hidden = panel.dataset.tab !== selectedKey;
    });
    if (remember) {
      try { sessionStorage.setItem('blah2-settings-tab', selectedKey); }
      catch (_) { /* Remembering a tab is optional. */ }
    }
  };
  activateSettingsTab = selectedKey => {
    const resolvedKey = selectedKey === 'location' ? 'capture' : selectedKey;
    if (!tabs.querySelector(`[data-tab="${resolvedKey}"]`)) return;
    activate(resolvedKey);
  };
  entries.forEach(([key, value]) => {
    const [title, description] = key === 'experimental' ?
      ['Experimental', 'Optional features'] :
      key === 'array' ? ['Array', 'Antennas and layout'] :
      key === 'truth' ? ['Aircraft', 'ADS-B sources'] :
      key === 'capture' ? ['Receiver', 'Radio and sites'] :
      key === 'process' ? ['Radar', 'Detection and tracking'] :
      key === 'save' ? ['Recording', 'Automatic clips and storage'] :
      key === 'network' ? ['Connectivity', 'Network and integrations'] : metadata([key]);
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.tab = key;
    button.setAttribute('role', 'tab');
    button.setAttribute('aria-controls', `settings-panel-${key}`);
    const strong = document.createElement('strong');
    const small = document.createElement('small');
    strong.textContent = title;
    small.textContent = description;
    small.hidden = true;
    button.title = description;
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
    if (key === 'array') { /* The visual editor is moved here after capture renders. */ }
    else if (key === 'location') panel.appendChild(renderSiteSettings());
    else if (key === 'save') panel.appendChild(renderRecordingSettings());
    else if (key === 'experimental') panel.appendChild(renderExperimentalSettings());
    else if (value !== null && typeof value === 'object' && !Array.isArray(value))
      panel.appendChild(group(key, value, [], true, {skipPath: path =>
        (key === 'process' && receiverSpecificProcessPath(path) &&
          (hasArrayTab || !/^process\.(reference_synthesis|surveillance_fusion|bearing)(\.|$)/.test(path))) ||
        experimentalSettingPath(path) ||
        (key === 'capture' && path.startsWith('capture.aircraft_clips')) ||
        (key === 'capture' && path.startsWith('capture.replay'))}));
    else
      panel.appendChild(field(value, [key]));
    if (key === 'capture') {
      panel.appendChild(renderSiteSettings());
      if (activeConfig.capture?.replay) panel.appendChild(group('replay', {
        recording_sha256: activeConfig.capture.replay.recording_sha256 ?? '',
        acquisition_sha256: activeConfig.capture.replay.acquisition_sha256 ?? ''
      }, ['capture'], false, {groupMeta: {'capture.replay': ['Replay site provenance',
        'Optional trust pins from an independent acquisition ledger. Without verified recorded sites, replay geography, truth association and fusion are unavailable. Live acquisition is unchanged.']}}));
      panel.appendChild(renderReceiverFeatures());
      if (!recorded && ['RspDuo', 'Usrp', 'HackRF'].includes(activeConfig.capture?.device?.type)) {
        const startup = document.createElement('section');
        startup.id = 'receiver-startup';
        startup.className = 'upstream-status receiver-startup';
        panel.appendChild(startup);
        renderReceiverStartup(null, startup);
      }
      if (!recorded && activeConfig.capture?.device?.type === 'RspDuo') {
        const health = document.createElement('section');
        health.id = 'receiver-health';
        health.className = 'upstream-status receiver-health';
        health.textContent = 'RSPduo overload status unavailable.';
        panel.appendChild(health);
        renderReceiverOverload(null, health);
      }
      if (!recorded) panel.appendChild(renderReceiverSetup());
      if (!recorded && upstream && receiverUsesUpstreamStatus(receiverType)) panel.appendChild(upstream);
    }
    panels.appendChild(panel);
  });
  moveArrayEditorToArrayTab(panels, hasArrayTab);
  revealSmallSettingsGroups(panels);
  panels.querySelectorAll('[role="tabpanel"]').forEach(panel =>
    organizeTechnicalControls(panel, panel.dataset.tab));
  target.append(makeSettingsFilter(tabs, panels, activate));
  target.append(tabs, panels);
  updateProcessingControlAvailability(target);
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

function receiverStartupShell(status, target, receiver, messages) {
  const processor = status?.processorFresh && status.processor?.receiver === receiver &&
    status.processor?.input === 'live' ? status.processor : null;
  const receipt = processor?.receiverStartup;
  const text = !processor ? `No current ${messages.name} status. Saved settings apply at startup.` :
    processor.state === 'error' ? `Receiver error: ${processor.error}${messages.errorHelp || ''}` :
    !receipt ? messages.unavailable :
    receipt.status !== 'accepted' ? `Applying settings through ${messages.sdk}…` :
    messages.qualification && !receipt[messages.qualification] ? messages.unqualified :
    processor.state === 'stopped' ? `Receiver stopped. ${messages.stopped}` :
    `${messages.accepted}; ${status.radar === 'receiving' ? 'receiving' : 'waiting for'} ${messages.frames || 'radar frames'}.`;
  let heading = target.querySelector('[data-receiver-startup-heading]');
  if (!heading) {
    heading = document.createElement('h3');
    heading.dataset.receiverStartupHeading = '';
    target.appendChild(heading);
  }
  heading.textContent = messages.title;
  let message = target.querySelector('[data-receiver-startup-message]');
  if (!message) {
    message = document.createElement('p');
    message.dataset.receiverStartupMessage = '';
    message.setAttribute('role', 'status');
    target.appendChild(message);
  }
  message.textContent = text;
  if (!receipt) {
    target.querySelector('[data-receiver-startup-mismatch]')?.remove();
    target.querySelector('[data-receiver-startup-details]')?.remove();
  }
  return receipt;
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
  const receipt = receiverStartupShell(status, target, 'RspDuo', {
    name: 'RSPduo', title: 'SDRplay startup', sdk: 'SDRplay',
    unavailable: 'Startup settings unavailable. Rebuild SDRplay support to see them.',
    stopped: 'SDRplay accepted the last startup settings.', accepted: 'SDRplay accepted settings'});
  if (!receipt) return;
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
  const receipt = receiverStartupShell(status, target, 'HackRF', {
    name: 'HackRF', title: 'Dual HackRF startup', sdk: 'libhackrf', qualification: 'setterAcceptance',
    unavailable: 'HackRF startup receipt unavailable. Rebuild HackRF support to see it.',
    unqualified: 'HackRF started without qualified setter acceptance.',
    stopped: 'The last HackRF startup calls were accepted.',
    accepted: 'Both HackRF startup paths accepted settings', frames: 'provisional paired frames'});
  if (!receipt) return;
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
  const receipt = receiverStartupShell(status, target, 'Usrp', {
    name: 'USRP', title: 'USRP startup', sdk: 'UHD', qualification: 'readbackAvailable',
    errorHelp: ' Reconnect the USRP if it was unplugged, then run vectorwarp restart.',
    unavailable: 'USRP startup readback unavailable. Rebuild UHD support to see it.',
    unqualified: 'USRP started without qualified UHD setting readback.',
    stopped: 'UHD read back the last startup settings.', accepted: 'UHD read back both channels'});
  if (!receipt) return;
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
  const check = document.createElement('button');
  check.type = 'button'; check.className = 'button-secondary';
  check.textContent = 'Scan for receivers';
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
      if (configurationBusy()) return;
      element.disabled = true;
      try { await callback(element); } catch (error) { paragraph(error.message); }
      finally { if (element.isConnected) element.disabled = false; }
    });
    output.appendChild(element);
    return element;
  };
  const sdrplayLink = (label = 'Install SDRplay support') => {
    const link = document.createElement('a');
    link.href = 'https://sdrplay.com/hardware-api/';
    link.className = 'button-secondary';
    link.textContent = label;
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
  async function reviewAction(receiverType, actionId) {
    if (serializeConfig(activeConfig) !== originalConfig)
      throw new Error('Save or discard your edits first. Receiver actions use saved settings.');
    const plan = await post('/api/receivers/plan', {receiverType, actionId});
    if (plan.status === 'not-required') { paragraph(plan.message); return; }
    if (plan.requiresAuthorization !== false)
      throw new Error('This installation cannot complete the action from the web app. Reinstall the complete VectorWarp package.');
    if (serializeConfig(activeConfig) !== originalConfig || configRevision !== plan.configRevision)
      throw new Error('Settings changed. Scan for receivers again.');
    const result = await post('/api/receivers/execute',
      {nonce: plan.nonce, configRevision: plan.configRevision}, 210000);
    paragraph(result.message);
    await discover();
  }
  async function runBuild() {
    const result = await post('/api/sdrplay-build', {}, 210000, 'sdrplay-local-build-v1');
    paragraph(result.message || 'SDRplay support is being prepared.');
    buildPolls = 0;
    refreshBuildStatus();
  }
  async function runAction(receiverType, actionId, element) {
    element.textContent = 'Working…';
    try {
      await reviewAction(receiverType, actionId);
    } catch (error) {
      throw new Error(`${error.message} Scan again before retrying.`);
    }
  }
  /*
   * Receiver installation is intentionally action-oriented. Do not add setup
   * guides or expose package paths here: a supported package is ready, a
   * button performs the bounded action, or the installation is incomplete.
   */
  async function discover() {
    if (buildPollTimer) { window.clearTimeout(buildPollTimer); buildPollTimer = null; }
    check.disabled = true;
    output.replaceChildren();
    paragraph('Checking receiver software…');
    try {
      const result = await post('/api/receivers/discover', {}, 30000, 'receiver-discovery-v2');
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
      if (!result.buildCapabilitiesKnown) paragraph('This VectorWarp installation is incomplete. Reinstall the current package.');
      for (const receiver of result.receivers || []) {
        const selected = receiver.type === activeConfig.capture?.device?.type;
        const ready = receiver.capabilities.liveCompiled && receiver.capabilities.runtimeLoadable !== false &&
          receiver.dependencies.state !== 'missing';
        const needsSdrplay = receiver.type === 'RspDuo' && receiver.dependencies.state !== 'installed';
        const incomplete = !receiver.capabilities.liveCompiled &&
          !(receiver.type === 'RspDuo' && receiver.capabilities.localBuildable);
        const card = document.createElement('article');
        card.className = `receiver-software-card${selected ? ' selected' : ''}`;
        card.dataset.receiverType = receiver.type;
        const cardHeader = document.createElement('header');
        const cardTitle = document.createElement('strong');
        cardTitle.textContent = receiver.label;
        const cardState = document.createElement('span');
        cardState.className = ready ? 'upstream-badge good' : 'upstream-badge warning';
        cardState.textContent = selected ? 'Selected' : receiver.capabilities.detected ? 'Detected' :
          needsSdrplay ? 'Install required' : ready ? 'Ready' : 'Unavailable';
        cardHeader.append(cardTitle, cardState);
        const state = document.createElement('p');
        state.textContent = needsSdrplay ? 'Install the SDRplay API once.' :
          incomplete ? 'This receiver is missing from the installed VectorWarp package.' :
          receiver.capabilities.detected ? 'Connected.' :
          ready ? 'Support installed.' :
          'Connect the receiver and scan again.';
        card.append(cardHeader, state);
        output.appendChild(card);
        if (receiver.type === 'RspDuo' && receiver.dependencies.state !== 'installed') {
          paragraph('Vendor license acceptance is required.');
          sdrplayLink();
        }
        if (receiver.type === 'RspDuo') {
          try {
            const response = await configFetch(liveApiUrl('/api/sdrplay-build'), {}, 5000);
            const build = await response.json();
            const relevantProgress = build.progress && (!build.progress.kit_id ||
              build.progress.kit_id === build.kit_id) ? build.progress : null;
            if (build.buildable && ['missing', 'stale'].includes(build.state)) {
              if (receiver.dependencies.state === 'installed') button('Install SDRplay support', runBuild);
            } else if (build.buildable && build.state === 'current') paragraph('SDRplay support is ready.');
            else if (!needsSdrplay && !build.buildable) paragraph('This VectorWarp installation is missing its SDRplay adapter kit. Reinstall the current package.');
            if (relevantProgress?.state === 'failed')
              paragraph(relevantProgress.reason || 'SDRplay support installation failed.');
            if (relevantProgress && ['queued', 'running'].includes(relevantProgress.state)) refreshBuildStatus();
          } catch (_) { if (!needsSdrplay) paragraph('SDRplay support could not be checked. Scan again.'); }
        }
        if (receiver.capabilities.liveCompiled ||
            (receiver.type === 'RspDuo' && receiver.capabilities.localBuildable)) button(`Use ${receiver.label}`, () => {
          switchDevice(receiver.type);
          paragraph(`${receiver.label} selected. Save to apply.`);
        });
        for (const action of result.management?.actions?.filter(item => item.receiverType === receiver.type) || []) {
          if (action.available && !action.ready) button(action.kind === 'start-service' ?
            (receiver.type === 'RspDuo' ? 'Start SDRplay' : 'Start receiver') : 'Install receiver software',
            (_element) => runAction(receiver.type, action.id, _element));
        }
      }
      if (result.processor?.state === 'error') paragraph(`Processor input error: ${result.processor.error}`);
      if (result.errors?.length) paragraph('One or more receiver checks failed. Scan again or send a diagnostic report.');
    } catch (error) { output.replaceChildren(); paragraph(error.message); }
    finally { check.disabled = false; }
  }
  check.addEventListener('click', discover);
  section.append(heading, check, output);
  window.setTimeout(() => { if (section.isConnected) discover(); }, 0);
  return section;
}

async function saveConfiguration(mode) {
  const saveLater = mode === 'pending';
  const button = document.getElementById(saveLater ? 'config-save-later' : 'config-save') ||
    document.getElementById('config-save');
  if (configurationBusy() || pendingLiveConfig || document.getElementById('site-dialog'))
    return false;
  configurationOperation = 'saving';
  updateDirtyState();
  button.textContent = 'Saving…';
  if (validationTimer) window.clearTimeout(validationTimer);
  let saved = false;
  let writeAttempted = false;
  let responseReceived = false;
  let fileOutcomeUnknown = false;
  try {
    if (!(await validateActiveConfiguration())) return false;
    const snapshot = serializeConfig(activeConfig);
    const body = JSON.stringify(activeConfig);
    let previousState = {};
    try { previousState = await readRadarState(); } catch (_) { /* Already offline. */ }
    if (snapshot !== serializeConfig(activeConfig) || snapshot !== validatedSnapshot) {
      showMessage('A setting changed during validation. Review it and save again.', 'warning');
      return false;
    }
    configurationOperation = 'restarting';
    const measuredGain = !saveLater && activeConfig.capture?.replay?.state !== true &&
      activeConfig.capture?.device?.type === 'Kraken' &&
      ['measured', 'measured_per_channel'].includes(
        activeConfig.capture?.device?.heimdall?.gain_control);
    const perChannelGain = measuredGain &&
      activeConfig.capture.device.heimdall.gain_control === 'measured_per_channel';
    setRestartProgress(30, saveLater ? 'Saving without applying…' : measuredGain ?
      perChannelGain ? 'Measuring receiver headroom and calibrating each channel…' :
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
    rememberOriginalConfig(activeConfig);
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
    configurationOperation = 'idle';
    refreshGeometryEditor();
    document.querySelectorAll('#config-fields input, #config-fields select').forEach(input => {
      input.disabled = input.dataset.originalDisabled === 'true';
    });
    updateDirtyState();
    if (saveLater) button.textContent = 'Save for later';
  }
  return saved;
}

async function syncConfigurationFromServer() {
  if (!activeConfig || configurationBusy() || configSyncInProgress || document.hidden || document.getElementById('site-dialog')) return;
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
    activeConfig = exposeLegacyTrackerProfile(await configResponse.json());
    configRevision = configResponse.headers.get('ETag')?.replace(/^"|"$/g, '');
    capabilities = capabilitiesResponse.ok ? await capabilitiesResponse.json() :
      {editable: false, restartAvailable: false, changesRequireRestart: true};
    const receiverSync = capabilities.receiverSynchronization || {};
    receiverApplyPending = ['unknown', 'saved-pending', 'partial', 'indeterminate', 'failed',
      'receiver-confirmed-config-not-saved'].includes(receiverSync.applicationState) &&
      !(receiverSync.applicationState === 'unknown' &&
        receiverSync.currentReceiver?.positiveAppliedAcknowledgement === true);
    rememberOriginalConfig(activeConfig);
    const replayParameters = new URLSearchParams(window.location.search);
    const replayFile = replayParameters.get('replayFile');
    replayLaunchPending = Boolean(replayFile);
    let replayConfigurationComplete = false;
    let replayLimitations = [];
    if (replayFile) {
      let recorded = {};
      const replayId = replayParameters.get('replayId');
      if (replayId) try {
        const response = await configFetch(liveApiUrl(`/api/recordings/${encodeURIComponent(replayId)}/replay-profile`),
          {cache: 'no-store'});
        if (response.ok) recorded = await response.json();
      } catch (_) { /* Header validation will still protect a legacy recording. */ }
      replayConfigurationComplete = recorded.configuration_complete === true;
      replayLimitations = Array.isArray(recorded.limitations) ? recorded.limitations : [];
      if (replayConfigurationComplete && recorded.profile) {
        const recordedProcess = recorded.profile.process || {};
        // Recorded processing is exact; hardware changes require an explicit edit.
        activeConfig.process = JSON.parse(JSON.stringify(recordedProcess));
        // Side-profile sites are not authenticated acquisition provenance.
        // Native replay supplies externally pinned recorded sites separately;
        // never overwrite the operator's live site configuration here.
        if (recorded.profile.capture?.array_geometry)
          activeConfig.capture.device.array_geometry = recorded.profile.capture.array_geometry;
      }
      if (activeConfig.capture.replay && activeConfig.capture.replay.file !== replayFile) {
        delete activeConfig.capture.replay.recording_sha256;
        delete activeConfig.capture.replay.acquisition_sha256;
      }
      activeConfig.capture.replay = {...activeConfig.capture.replay,
        state: true, file: replayFile, format: 'auto',
        ...Object.fromEntries(Object.entries({sample_rate_hz: recorded.sample_rate_hz,
          frequency_hz: recorded.frequency_hz, channel_count: recorded.channel_count,
          reference_channel: recorded.reference_channel, reference_mode: recorded.reference_mode,
          surveillance_channels: recorded.surveillance_channels,
          reference_channels: recorded.reference_channels})
          .filter(([, value]) => value !== null && value !== undefined))};
      try { sessionStorage.setItem('blah2-settings-tab', 'capture'); } catch (_) {}
      if (window.history?.replaceState)
        window.history.replaceState(null, '', window.location.pathname);
    }
    validationErrors = [];
    validationPending = false;
    target.innerHTML = `<div class="config-toolbar"><div><strong>Unsaved settings</strong><span id="config-state"></span></div><div class="config-actions"><button id="config-reset" class="button-secondary" type="button" disabled>Discard changes</button><button id="config-save" class="button-primary" type="button" disabled>Save & Restart</button></div></div><div id="config-message" class="config-message"></div><section id="upstream-status" class="upstream-status"><div class="upstream-header"><strong>Upstream receiver check</strong><span class="upstream-badge">Checking…</span></div></section><div id="config-restart-progress" class="config-restart-progress" role="status" aria-live="polite" hidden><div class="config-restart-copy"><span id="config-restart-label">Preparing restart…</span><b id="config-restart-value">0%</b></div><div class="config-restart-track"><span id="config-restart-bar"></span></div></div><div id="config-fields" class="config-editor"></div>`;
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
    if (replayFile) {
      const title = document.createElement('strong');
      const copy = document.createElement('span');
      title.textContent = replayConfigurationComplete ? ' Recording configuration loaded.' :
        ' Legacy recording configuration is incomplete.';
      copy.textContent = replayConfigurationComplete ?
        ' Start replay to use the recorded processing profile, frequency and sample rate. Geographic association/fusion requires externally trusted recorded acquisition sites; live sites are never a fallback. Review hardware settings explicitly if this machine differs.' :
        ` Only recording-header settings could be loaded. ${replayLimitations.join(' ')}`;
      setup.append(title, copy);
      const save = document.getElementById('config-save');
      save.textContent = 'Start replay';
      if (!replayConfigurationComplete) save.title = 'Review legacy replay settings before starting.';
    } else if (capabilities.setupRequired) {
      const title = document.createElement('strong');
      const copy = document.createElement('span');
      title.textContent = ' Setup required.';
      copy.textContent = ' Complete the setup assistant before starting.';
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
    document.getElementById('config-save').addEventListener('click', saveConfiguration);
    document.getElementById('config-reset').addEventListener('click', () => {
      if (validationTimer) window.clearTimeout(validationTimer);
      validationSequence += 1;
      validationErrors = [];
      validatedSnapshot = null;
      if (pendingLiveConfig !== null) {
        activeConfig = pendingLiveConfig;
        rememberOriginalConfig(pendingLiveConfig);
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
    window.renderLocalNodes?.();
    if (replayFile)
      showMessage('Recording selected. Start replay to use its saved configuration; the live SDR stays closed.', 'warning');
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
  if (configurationOperation === 'restarting') { renderReceiverOverload(null); return; }
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
  module.exports = {applyDeviceProfile, metadata, normalizeKrakenChannels, receiverFeatureCapabilities, processingControlCapabilities,
    experimentalSettingCapability,
    applyIlluminatorStandardDefaults, ATSC_CHANNELIZER_DEFAULTS,
    receiverSpecificProcessPath, delaySampleDescription, adsbExchangeId, adsbPublicSource,
    serializeConfig, upstreamRestartError, receiverSaveMessage, receiverFailureMessage, receiverMayHaveChanged,
    receiverSaveDetails, receiverUsesUpstreamStatus, accelerationSummary,
    CONFIG_SAVE_TIMEOUT_MS, MEASURED_GAIN_SAVE_TIMEOUT_MS};
