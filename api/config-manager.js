const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const net = require('net');
const crypto = require('crypto');
const {sourceAddress} = require('./adsb-source');
const {classifyAdsbSource, SOURCE_CHOICES} = require('./adsb-discovery');
const {recordErrors} = require('../html/js/kraken_geometry');
const {evaluateKrakenArray} = require('./kraken-array');
const {effectiveDelayConfig, temporalConfirmationErrors} = require('../html/js/config_profiles');
const {processingCapabilities, residentReferenceCompatible, regularSurveillancePolygon,
  residentRouteRequested, experimentalSettingSupport} = require('../html/js/processing_capabilities');
const FIELD_RULES = require('./config-rules')(SOURCE_CHOICES);

// Upgrade-only values, absent from the advertised settings contract. Keep
// refusal for unsupported alternatives instead of silently changing intent.
const RETIRED_FIXED_SETTINGS = Object.freeze({
  'save.iq': value => typeof value === 'boolean',
  'save.timing': value => typeof value === 'boolean',
  'network.ports.config': value => Number.isInteger(value) && value > 0 && value <= 65535,
  'process.multi_illuminator.input_phase_correction': value => value === 'off',
  'process.multi_illuminator.doppler_window': value => value === 'rectangular',
  'process.bearing.candidate_peaks': value => value === 3
});
function publicationPolicyError(config) {
  const publication = config?.process?.tracker?.publication;
  if (publication === undefined || publication?.mode === 'native') return null;
  return 'process.tracker.publication is retired: external/hybrid tracks are not qualified. Remove the publication block to explicitly select canonical native tracking; no replacement stream has been activated.';
}

// Admission mirror of detection::temporalIntegrationBudget. These dimensions
// are the native lane map, not capture-wide or presentation-decimated geometry.
function temporalBudgetError(rows, columns, detection) {
  const tbd = detection.track_before_detect, temporal = tbd?.enable === true;
  const depth = temporal ? (tbd.max_cpis ?? 8) : (detection.integration_cpis ?? 1);
  if (detection.enable !== true || depth === 1) return null;
  const delay = temporal ? (tbd.delay_motion_bins ?? tbd.motion_bins ?? 1) :
    (detection.integration_motion_bins ?? 0);
  const doppler = temporal ? (tbd.doppler_motion_bins ?? tbd.motion_bins ?? 1) : delay;
  if (![rows, columns, depth, delay, doppler].every(Number.isSafeInteger) ||
      rows < 1 || columns < 1 || ![2, 4, 8].includes(depth) || delay < 0 || doppler < 0)
    return null; // The field/geometry validators report malformed intent.
  const cells = rows * columns;
  const workingBytes = cells * (depth * 8 + 192 + (depth === 8 ? 4 : depth === 4 ? 3 : 2) * 8);
  const pathSamples = cells * (2 * delay + 1) * (2 * doppler + 1) *
    (depth === 8 ? 14 : depth === 4 ? 6 : 2);
  return cells > 1048576 || workingBytes > 268435456 || pathSamples > 268435456 ?
    `temporal detector native map ${rows}x${columns} requires ${workingBytes} bytes and ${pathSamples} path samples; limits are 1048576 cells, 268435456 bytes and 268435456 path samples` : null;
}

const KRAKEN_MAX_CHANNELS = 8;
const UINT32_MAX = 4294967295;
const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;
const DEVICE_PROFILES = [
  {
    type: 'Kraken',
    label: 'KrakenSDR Suite V2',
    description: '2–8 coherent channels streamed by HeIMDALL',
    features: {
      coherentArray: true,
      residentIntegration: true,
      bearing: true,
      manifold: true
    },
    sampleRate: 2400000,
    device: {
      type: 'Kraken',
      heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092,
        gain: 'keep', gain_control: 'measured_per_channel'},
      channel_count: 5,
      reference_channel: 0,
      surveillance_channels: [0, 1, 2, 3, 4]
    },
    process: {
      performance: {surveillance_workers: 0, fft_threads: 0},
      reference_synthesis: {
        mode: 'array_eigenbeam',
        channels: [0, 1, 2, 3, 4],
        analysis_samples: 32768,
        analysis_interval: 10,
        power_iterations: 12,
        covariance_smoothing: 0.8,
        diagonal_loading: 0.001,
        target_protection_enabled: false, target_protection_beams: 12,
        target_protection_beam_index: 0
      },
      surveillance_fusion: {
        mode: 'noncoherent', beams: 12, effective_beam_trials: 12,
        direct_path_null_db: 0
      }
    }
  },
  {
    type: 'RspDuo',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'SDRplay RSPduo',
    description: 'Two coherent tuners in dual-tuner mode',
    features: {residentIntegration: true},
    sampleRate: 2000000,
    device: {
      type: 'RspDuo', agcSetPoint: -20, bandwidthNumber: 5,
      serial: '',
      gainReduction: [50, 45], lnaState: 1,
      dabNotch: false, rfNotch: false
    }
  },
  {
    type: 'Usrp',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'Ettus USRP',
    description: 'Two coherent receive channels through UHD',
    features: {residentIntegration: true},
    sampleRate: 2000000,
    device: {
      type: 'Usrp', address: 'type=b200', subdev: 'A:A A:B',
      timing_source: 'internal',
      antenna: ['RX2', 'RX2'], gain: [20, 20]
    }
  },
  {
    type: 'HackRF',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'Dual HackRF',
    description: 'Two synchronized HackRF receivers',
    features: {residentIntegration: true},
    sampleRate: 2000000,
    device: {
      type: 'HackRF',
      serial: ['REFERENCE_DEVICE_SERIAL_NUMBER',
        'SURVILLANCE_DEVICE_SERIAL_NUMBER'],
      gain_lna: [32, 32], gain_vga: [30, 30],
      amp_enable: [false, false]
    }
  }
];

function getDeviceProfiles() {
  return JSON.parse(JSON.stringify(DEVICE_PROFILES));
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function canonicalPlanJson(value) {
  if (Array.isArray(value)) return '[' + value.map(canonicalPlanJson).join(',') + ']';
  if (isObject(value)) return '{' + Object.keys(value).filter(key => value[key] !== undefined)
    .sort().map(key => JSON.stringify(key) + ':' + canonicalPlanJson(value[key])).join(',') + '}';
  if (typeof value === 'number' && !Number.isFinite(value)) throw new Error('Non-finite radar plan value');
  if (value === undefined) throw new Error('Missing radar plan value');
  return JSON.stringify(value);
}
const planSha256 = value => crypto.createHash('sha256').update(canonicalPlanJson(value)).digest('hex');

function effectiveRadarPlan(config) {
  const p = config.process || {}, capture = config.capture || {};
  const replay = capture.replay?.state === true ? capture.replay : {};
  const topology = processingCapabilities(config);
  const multi = p.multi_illuminator?.enabled === true;
  const lanes = multi ? (p.multi_illuminator.illuminators || []).filter(lane => lane.enabled === true) :
    [{id: 'primary', output_sample_rate_hz: replay.sample_rate_hz ?? capture.fs,
      frequency_hz: replay.frequency_hz ?? capture.fc}];
  if (!lanes.length) throw new Error('Radar plan has no enabled lanes');
  const plans = lanes.map(lane => {
    const rate = lane.output_sample_rate_hz;
    const process = JSON.parse(JSON.stringify(effectiveDelayConfig(config, rate,
      {scaleNeighborhoods: true}).process));
    for (const name of ['performance', 'evidence', 'multi_illuminator', 'delay_reference_sample_rate_hz'])
      delete process[name];
    delete process.data.buffer;
    delete process.tracker.profile;
    delete process.tracker.gpu_capacity;
    delete process.bearing?.manifold_recorder;
    const reference = process.reference_synthesis = {...process.reference_synthesis,
      mode: lane.reference_channel === undefined ? topology.referenceMode : 'dedicated',
      channels: lane.reference_channel === undefined ? topology.referenceChannels : [lane.reference_channel]};
    const detection = process.detection, tbd = detection.track_before_detect || {};
    const depth = tbd.enable === true ? (tbd.max_cpis ?? 8) : (detection.integration_cpis ?? 1);
    const motion = tbd.enable === true ? (tbd.motion_bins ?? 1) : (detection.integration_motion_bins ?? 0);
    const features = {clutter: process.clutter.enable === true,
      detection: detection.enable === true,
      tracking: detection.enable === true && process.tracker.enable === true,
      bearing: process.bearing?.enable === true,
      temporal_cpis: detection.enable === true ? depth : 0,
      delay_motion_bins: tbd.enable === true ? (tbd.delay_motion_bins ?? motion) : motion,
      doppler_motion_bins: tbd.enable === true ? (tbd.doppler_motion_bins ?? motion) : motion,
      fractional_delay: tbd.enable === true && tbd.fractional_delay === true};
    for (const key of ['track_before_detect', 'integration_cpis', 'integration_motion_bins']) delete detection[key];
    detection.temporal = {cpis: depth, delay_motion_bins: features.delay_motion_bins,
      doppler_motion_bins: features.doppler_motion_bins, fractional_delay: features.fractional_delay};
    if (process.clutter.followAmbiguity === true) {
      process.clutter.delayMin = process.ambiguity.delayMin;
      process.clutter.delayMax = process.ambiguity.delayMax + 1;
    }
    delete process.clutter.followAmbiguity;
    const signal = {sample_rate_hz: rate, frequency_hz: lane.frequency_hz,
      channel_count: topology.channelCount,
      reference_channel: lane.reference_channel ?? topology.referenceChannel,
      surveillance_channels: lane.surveillance_channels ?? topology.surveillanceChannels,
      array_geometry: capture.device?.array_geometry,
      transmitter_location: lane.transmitter ?? config.location?.tx,
      radar_transmitter_location: lane.transmitter_location,
      receiver_location: config.location?.rx};
    // Bind geometry, not site labels.
    for (const field of ['transmitter_location', 'radar_transmitter_location', 'receiver_location'])
      if (signal[field]) signal[field] = Object.fromEntries(['latitude', 'longitude', 'altitude']
        .map(key => [key, signal[field][key]]));
    // Match canonical ATSC conditioning.
    const atsc1 = ['ATSC', 'ATSC-1.0'].includes(lane.standard);
    const referenceMode = lane.atsc_reference_mode ?? (atsc1 || lane.standard === 'ATSC-3.0' ? 'automatic' : 'raw');
    if (referenceMode === 'pilot_aided' || (referenceMode === 'automatic' && atsc1 &&
        reference.mode !== 'array_eigenbeam'))
      reference.atsc_reference_mode = 'pilot_aided';
    if (lane.standard === 'FM' && p.multi_illuminator?.experimental_fm_reference_quality_filter === true)
      reference.fm_quality_filter = true;
    if (lane.tracker_min_snr_db !== undefined) process.tracker.initiate.min_snr_db = lane.tracker_min_snr_db;
    for (const field of ['detection_manifold_file', 'bearing_manifold_file'])
      if (lane[field] !== undefined) signal[field] = lane[field];
    return {id: lane.id, features, semantic: {signal, process},
      configured_products: {'stages.clutter': features.clutter, 'stages.detection': features.detection,
        'stages.tracking': features.tracking, integrationCpis: depth,
        trackerM: process.tracker.initiate.M, trackerN: process.tracker.initiate.N,
        trackerDelete: process.tracker.delete}};
  });
  const semantic = {schema: 'vectorwarp-configured-radar-plan-v1',
    lanes: plans.map(lane => canonicalPlanJson(lane.semantic)).sort().map(value => JSON.parse(value))};
  return {schema: 'vectorwarp-radar-plan-report-v1',
    scope: 'configured-intent; native execution and outcome evidence required separately',
    semantic_sha256: planSha256(semantic),
    semantic, lanes: plans.map(({semantic, ...lane}) =>
      ({...lane, configured_sha256: planSha256(semantic)}))};
}

function radarPlanErrors(report, requirement) {
  if (!isObject(requirement) || requirement.schema !== 'vectorwarp-radar-plan-requirements-v1' ||
      !/^[a-f0-9]{64}$/.test(requirement.semantic_sha256 || '') ||
      !isObject(requirement.required_features) || !Object.keys(requirement.required_features).length ||
      Object.keys(requirement).some(key => !['schema', 'semantic_sha256', 'required_features', 'resolved_products'].includes(key)) ||
      (requirement.resolved_products !== undefined && (!isObject(requirement.resolved_products) ||
        Object.entries(requirement.resolved_products).flat().some(value => !/^[a-f0-9]{64}$/.test(value)))))
    return ['Required radar plan is malformed'];
  const errors = [];
  if (report.semantic_sha256 !== requirement.semantic_sha256)
    errors.push('Effective radar plan differs from the independently pinned intended plan');
  for (const [feature, expected] of Object.entries(requirement.required_features)) {
    for (const lane of report.lanes) {
      if (!Object.hasOwn(lane.features, feature) || lane.features[feature] !== expected)
        errors.push(`Required radar feature ${feature}=${JSON.stringify(expected)} is absent in lane ${lane.id} (configured ${feature}=${JSON.stringify(lane.features[feature])})`);
    }
  }
  if (requirement.resolved_products !== undefined && report.lanes.some(lane =>
      !requirement.resolved_products[lane.configured_sha256]))
    errors.push('Required native resolved plan is missing for a configured lane');
  return errors;
}

function assessResolvedRadarPlans(configured, observation, {now = Date.now(), maxAgeMs = 5000} = {}) {
  const errors = [...(configured.errors || [])], lanes = [];
  const observedProducts = Array.isArray(observation?.products) ? observation.products : [];
  const reject = message => errors.push(message);
  if (configured.qualification !== 'matched') reject('Configured radar plan is not independently pinned');
  if (observation?.schema !== 'vectorwarp-runtime-plan-observation-v1' ||
      !Number.isSafeInteger(observation.generation) || observation.generation < 0 ||
      !Number.isFinite(observation.observed_at) || observation.observed_at > now ||
      now - observation.observed_at > maxAgeMs || observation.processor_fresh !== true ||
      !['live', 'replay'].includes(observation.input) || !Array.isArray(observation.products))
    reject('Native radar plan observation is missing, stale or outside an active processor session');
  for (const lane of configured.lanes || []) {
    const products = observedProducts.filter(item => item?.lane_id === lane.id);
    const expectedHash = configured.resolved_products?.[lane.configured_sha256];
    if (!expectedHash) reject(`Lane ${lane.id} has no independently qualified native plan pin`);
    const required = ['map', ...(lane.features.detection ? ['detection'] : []),
      ...(lane.features.tracking ? ['tracker'] : []), ...(lane.features.bearing ? ['bearing'] : [])];
    let lineage = null, nativeHash = null;
    for (const name of required) {
      const matches = products.filter(item => item.product === name);
      const item = matches[0], frame = item?.frame;
      if (matches.length !== 1 || item.generation !== observation.generation ||
          !Number.isFinite(item.received_at) || item.received_at > observation.observed_at ||
          now - item.received_at > maxAgeMs || !isObject(frame)) {
        reject(`Lane ${lane.id} ${name} has no fresh native plan product`); continue;
      }
      let native;
      try {
        if (typeof frame.resolved_product_plan !== 'string' || frame.resolved_product_plan.length > 32768)
          throw new Error('missing identity');
        native = JSON.parse(frame.resolved_product_plan);
        if (native?.schema !== 'vectorwarp-resolved-products-v1') throw new Error('unknown schema');
      } catch (_) { reject(`Lane ${lane.id} ${name} lacks the canonical native resolved plan`); continue; }
      const hash = crypto.createHash('sha256').update(frame.resolved_product_plan).digest('hex');
      if (hash !== expectedHash) reject(`Lane ${lane.id} ${name} native plan differs from its qualified pin`);
      if (nativeHash && nativeHash !== hash) reject(`Lane ${lane.id} mixes native product plans`);
      nativeHash = hash;
      for (const [field, expected] of Object.entries(lane.configured_products || {}))
        if (native[field] !== expected) reject(`Lane ${lane.id} native ${field} differs from configured intent`);
      const input = frame.input_lineage;
      const exact = value => typeof value === 'string' && /^(0|[1-9][0-9]{0,19})$/.test(value) &&
        BigInt(value) <= 18446744073709551615n;
      if (input?.available !== true || input.sampleInterval !== 'half-open' ||
          !['captureEpochExact', 'firstInputSampleExact', 'endInputSampleExact']
          .every(key => exact(input[key])) || BigInt(input.endInputSampleExact) <= BigInt(input.firstInputSampleExact)) {
        reject(`Lane ${lane.id} ${name} lacks exact input lineage`); continue;
      }
      const identity = [input.captureEpochExact, input.firstInputSampleExact, input.endInputSampleExact].join(':');
      if (lineage && lineage !== identity) reject(`Lane ${lane.id} products do not share one input interval`);
      lineage = identity;
      const stages = ['fusion', ...(lane.features.temporal_cpis > 1 ? ['temporal'] : []),
        ...(lane.features.detection ? ['detector'] : []), ...(lane.features.tracking ? ['tracker'] : []),
        ...(lane.features.bearing ? ['bearing'] : [])];
      for (const stage of stages) {
        const receipt = frame.product_stage_execution?.[stage], execution = receipt?.execution;
        if (!['cpu', 'vulkan', 'vulkan-resident', 'vulkan-temporal'].includes(receipt?.backend) ||
            execution?.known !== true || execution.frameBound !== true || execution.available !== true ||
            execution.sampleInterval !== 'half-open' || execution.provenance !== 'processor_stage_receipt' ||
            (receipt.backend === 'cpu' ? execution.cpuExecuted !== true : execution.gpuExecuted !== true) ||
            ['captureEpochExact', 'firstInputSampleExact', 'endInputSampleExact']
              .some(key => execution[key] !== input[key]))
          reject(`Lane ${lane.id} ${name} lacks frame-bound ${stage} execution`);
      }
    }
    lanes.push({id: lane.id, configured_sha256: lane.configured_sha256, native_sha256: nativeHash, input_interval: lineage});
  }
  return {schema: 'vectorwarp-runtime-plan-acceptance-v1',
    scope: 'native plan and stage reporting; numerical parity and radar outcomes require separate qualification',
    qualification: errors.length ? 'rejected' : 'matched', errors: [...new Set(errors)], lanes};
}

function linuxServicePathPolicy(environment = process.env) {
  if (environment.VECTORWARP_LINUX_SERVICE !== '1') return null;
  return Object.freeze({kind: 'systemd-linux',
    recordingRoot: '/var/lib/vectorwarp/recordings',
    hiddenReplayRoots: Object.freeze(['/home', '/root', '/run/user', '/tmp', '/var/tmp'])});
}

function pathWithin(root, candidate) {
  const relative = path.relative(path.resolve(root), path.resolve(candidate));
  return relative === '' || (!relative.startsWith(`..${path.sep}`) &&
    relative !== '..' && !path.isAbsolute(relative));
}

function nearestExistingRealPath(candidate) {
  let current = path.resolve(candidate);
  while (!fs.existsSync(current)) {
    const parent = path.dirname(current);
    if (parent === current) return null;
    current = parent;
  }
  try { return fs.realpathSync(current); }
  catch (_) { return null; }
}

function servicePathWithin(root, candidate) {
  if (!pathWithin(root, candidate)) return false;
  if (!fs.existsSync(root)) return true;
  const realRoot = nearestExistingRealPath(root);
  const realCandidate = nearestExistingRealPath(candidate);
  return !realRoot || !realCandidate || pathWithin(realRoot, realCandidate);
}

function servicePathHidden(roots, candidate) {
  if (roots.some(root => pathWithin(root, candidate))) return true;
  const realCandidate = nearestExistingRealPath(candidate);
  return realCandidate !== null && roots.some(root => {
    if (!fs.existsSync(root)) return false;
    const realRoot = nearestExistingRealPath(root);
    return realRoot !== null && pathWithin(realRoot, realCandidate);
  });
}

function validateConfig(config, baseline = null, options = {}) {
  const errors = [];
  const notices = [];
  const pathPolicy = options.pathPolicy === undefined ?
    linuxServicePathPolicy(options.environment || process.env) : options.pathPolicy;
  const advise = (field, message) => notices.push({field, message});
  const inspectKeys = (value, name = 'configuration') => {
    if (name === 'configuration.capture.device.array_geometry') return;
    if (Array.isArray(value)) {
      value.forEach((item, index) => inspectKeys(item, `${name}[${index}]`));
      return;
    }
    if (!isObject(value)) return;
    Object.entries(value).forEach(([key, child]) => {
      if (key === '__proto__' || key === 'prototype' || key === 'constructor')
        errors.push(`${name}.${key} is not allowed`);
      inspectKeys(child, `${name}.${key}`);
    });
  };
  const requireObject = (value, name) => {
    if (!isObject(value)) errors.push(`${name} must be an object`);
    return isObject(value) ? value : {};
  };
  const number = (value, name, options = FIELD_RULES[name] || {}) => {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      errors.push(`${name} must be a finite number`);
      return null;
    }
    if (options.integer && !Number.isInteger(value))
      errors.push(`${name} must be an integer`);
    if (options.min !== undefined && value < options.min)
      errors.push(`${name} must be at least ${options.min}`);
    if (options.max !== undefined && value > options.max)
      errors.push(`${name} must be at most ${options.max}`);
    if (options.exclusiveMin !== undefined && value <= options.exclusiveMin)
      errors.push(`${name} must be greater than ${options.exclusiveMin}`);
    if (options.exclusiveMax !== undefined && value >= options.exclusiveMax)
      errors.push(`${name} must be smaller than ${options.exclusiveMax}`);
    return value;
  };
  const oneOf = (value, name, allowed = FIELD_RULES[name]?.choices) => {
    if (!allowed.includes(value))
      errors.push(`${name} must be one of ${allowed.join(', ')}`);
  };
  const boolean = (value, name) => {
    if (typeof value !== 'boolean') errors.push(`${name} must be true or false`);
  };
  const string = (value, name) => {
    if (typeof value !== 'string' || value.trim() === '')
      errors.push(`${name} must be a non-empty string`);
  };
  const integerArray = (value, name) => {
    if (!Array.isArray(value) || value.length === 0) {
      errors.push(`${name} must be a non-empty list`);
      return [];
    }
    value.forEach((item, index) => number(item, `${name}[${index}]`,
      {integer: true, min: 0}));
    if (new Set(value).size !== value.length)
      errors.push(`${name} must not contain duplicate channels`);
    return value;
  };

  const root = requireObject(config, 'configuration');
  inspectKeys(root);
  // Validate known fields independently of the current file's types. A broken
  // file must be repairable; an optional supported setting may be added.
  const inspectFields = (value, keys = []) => {
    const key = keys.join('.');
    // Optional operator metadata has its own bounded schema, including drafts.
    if (key === 'capture.device.array_geometry') return;
    // Former external converter address: ignored during migration and never
    // emitted by the in-process VectorWarp converter.
    if (key === 'truth.adsb.adsb2dd') return;
    if (RETIRED_FIXED_SETTINGS[key]) {
      if (!RETIRED_FIXED_SETTINGS[key](value))
        errors.push(`${key} is retired and its requested alternative is unsupported`);
      return;
    }
    if (key === 'process.tracker.publication') {
      const error = publicationPolicyError(root);
      if (error) errors.push(error);
      return;
    }
    const rule = FIELD_RULES[key];
    const previous = keys.reduce((item, name) => item?.[name], baseline);
    if (rule && rule.type === 'serial' && !(typeof value === 'string' || Array.isArray(value)))
      errors.push(`${key} must be a string or array`);
    else if (rule && rule.type === 'gain' && !(value === 'keep' || value === -1 ||
        (typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 50 && Math.round(value * 10) === value * 10)))
      errors.push(`${key} must be keep, automatic (-1), or 0 through 50 dB in 0.1 dB steps`);
    else if (rule && rule.type === 'autoBoolean' && !(typeof value === 'boolean' ||
        ['auto', 'on', 'off'].includes(value)))
      errors.push(`${key} must be auto, on, off, or a boolean`);
    else if (rule && (rule.type === 'array' ? !Array.isArray(value) : !['gain', 'serial', 'autoBoolean'].includes(rule.type) && typeof value !== rule.type))
      errors.push(`${key} must be ${rule.type}`);
    if (isObject(value)) {
      if (key && !rule && !Object.keys(FIELD_RULES).some(field => field.startsWith(`${key}.`)) &&
          (!baseline || JSON.stringify(previous) !== JSON.stringify(value)))
        errors.push(`${key} is not a supported setting`);
      Object.entries(value).forEach(([name, child]) => inspectFields(child, [...keys, name]));
      return;
    }
    if (!rule) {
      if (!baseline || JSON.stringify(previous) !== JSON.stringify(value))
        errors.push(`${key} is not a supported setting`);
      return;
    }
    if (rule.type === 'number') number(value, key, rule);
    if (rule.choices && !(rule.type === 'autoBoolean' && typeof value === 'boolean'))
      oneOf(value, key, rule.choices);
    // Arrays and objects returned by GET are distinct JavaScript references
    // when submitted back through Apply.  Read-only means value equality, not
    // object identity; otherwise an unchanged ordered ADS-B source list makes
    // every unrelated live configuration change impossible.
    if (rule.readOnly && baseline && (previous !== undefined || rule.migrationOnly) &&
        JSON.stringify(previous) !== JSON.stringify(value))
      errors.push(`${key} is read-only: ${rule.readOnly}`);
  };
  inspectFields(root);
  const capture = requireObject(root.capture, 'capture');
  const aircraftClips = capture.aircraft_clips === undefined ? null :
    requireObject(capture.aircraft_clips, 'capture.aircraft_clips');
  const device = requireObject(capture.device, 'capture.device');
  const replay = requireObject(capture.replay, 'capture.replay');
  const residentSnapshot = replay.resident_snapshot === undefined ? null :
    requireObject(replay.resident_snapshot, 'capture.replay.resident_snapshot');
  const processConfig = requireObject(root.process, 'process');
  const residentMode = residentRouteRequested(root);
  const evidence = processConfig.evidence === undefined ? null :
    requireObject(processConfig.evidence, 'process.evidence');
  const causalFunnel = evidence?.causal_funnel === undefined ? null :
    requireObject(evidence.causal_funnel, 'process.evidence.causal_funnel');
  const data = requireObject(processConfig.data, 'process.data');
  const multiIlluminator = processConfig.multi_illuminator === undefined ? null :
    requireObject(processConfig.multi_illuminator, 'process.multi_illuminator');
  let ambiguity = requireObject(processConfig.ambiguity, 'process.ambiguity');
  let clutter = requireObject(processConfig.clutter, 'process.clutter');
  let detection = requireObject(processConfig.detection, 'process.detection');
  const tracker = requireObject(processConfig.tracker, 'process.tracker');
  if (causalFunnel?.enabled === true &&
      (!residentMode || multiIlluminator?.enabled === true || tracker.enable !== true))
    errors.push('Native causal diagnostics require the single-lane resident tracker with auto or gpu-resident acceleration');
  const initiate = requireObject(tracker.initiate, 'process.tracker.initiate');
  const network = requireObject(root.network, 'network');
  const ports = requireObject(network.ports, 'network.ports');
  const trackerForward = network.tracker_forward === undefined ? null :
    requireObject(network.tracker_forward, 'network.tracker_forward');
  const unassistedTrackerForward = trackerForward?.unassisted === undefined ? null :
    requireObject(trackerForward.unassisted, 'network.tracker_forward.unassisted');
  const location = requireObject(root.location, 'location');
  const receiver = requireObject(location.rx, 'location.rx');
  const transmitter = requireObject(location.tx, 'location.tx');
  const save = requireObject(root.save, 'save');

  if (causalFunnel) {
    boolean(causalFunnel.enabled, 'process.evidence.causal_funnel.enabled');
    if (causalFunnel.api_ingest_enabled !== undefined)
      boolean(causalFunnel.api_ingest_enabled,
        'process.evidence.causal_funnel.api_ingest_enabled');
    if (typeof causalFunnel.epoch_id !== 'string' ||
        !/^[A-Za-z0-9][A-Za-z0-9._-]{7,63}$/.test(causalFunnel.epoch_id))
      errors.push('process.evidence.causal_funnel.epoch_id must be an 8–64 character safe immutable epoch ID');
    string(causalFunnel.path, 'process.evidence.causal_funnel.path');
    if (typeof causalFunnel.path === 'string' && !path.isAbsolute(causalFunnel.path))
      errors.push('process.evidence.causal_funnel.path must be absolute');
    number(causalFunnel.max_candidates, 'process.evidence.causal_funnel.max_candidates');
    number(causalFunnel.max_queue, 'process.evidence.causal_funnel.max_queue');
    number(causalFunnel.max_queue_mb, 'process.evidence.causal_funnel.max_queue_mb');
    number(causalFunnel.max_file_mb, 'process.evidence.causal_funnel.max_file_mb');
    number(causalFunnel.max_files, 'process.evidence.causal_funnel.max_files');
    if (causalFunnel.enabled === true &&
        (!Number.isInteger(ports.causal_funnel) || ports.causal_funnel < 1 || ports.causal_funnel > 65535))
      errors.push('network.ports.causal_funnel is required when the causal funnel is enabled');
  }

  number(capture.fs, 'capture.fs');
  number(capture.fc, 'capture.fc');
  if (aircraftClips) {
    boolean(aircraftClips.enabled, 'capture.aircraft_clips.enabled');
    number(aircraftClips.pre_seconds, 'capture.aircraft_clips.pre_seconds');
    number(aircraftClips.post_seconds, 'capture.aircraft_clips.post_seconds');
    const unmatchedMotion = aircraftClips.unmatched_motion === undefined ? null :
      requireObject(aircraftClips.unmatched_motion,
        'capture.aircraft_clips.unmatched_motion');
    if (unmatchedMotion) {
      boolean(unmatchedMotion.enabled,
        'capture.aircraft_clips.unmatched_motion.enabled');
      number(unmatchedMotion.minimum_similarity, 'capture.aircraft_clips.unmatched_motion.minimum_similarity');
      boolean(unmatchedMotion.allow_kinematic_fallback,
        'capture.aircraft_clips.unmatched_motion.allow_kinematic_fallback');
      number(unmatchedMotion.minimum_measured_fraction,
        'capture.aircraft_clips.unmatched_motion.minimum_measured_fraction');
      number(unmatchedMotion.minimum_delay_motion_km,
        'capture.aircraft_clips.unmatched_motion.minimum_delay_motion_km');
      number(unmatchedMotion.minimum_net_delay_motion_km,
        'capture.aircraft_clips.unmatched_motion.minimum_net_delay_motion_km');
      number(unmatchedMotion.maximum_delay_fit_rms_km,
        'capture.aircraft_clips.unmatched_motion.maximum_delay_fit_rms_km');
      number(unmatchedMotion.maximum_kinematic_error_mps,
        'capture.aircraft_clips.unmatched_motion.maximum_kinematic_error_mps');
      number(unmatchedMotion.minimum_measured_cpis,
        'capture.aircraft_clips.unmatched_motion.minimum_measured_cpis');
      number(unmatchedMotion.minimum_duration_seconds,
        'capture.aircraft_clips.unmatched_motion.minimum_duration_seconds');
      number(unmatchedMotion.minimum_absolute_doppler_hz,
        'capture.aircraft_clips.unmatched_motion.minimum_absolute_doppler_hz');
      number(unmatchedMotion.minimum_interval_seconds,
        'capture.aircraft_clips.unmatched_motion.minimum_interval_seconds');
    }
    if (Number.isFinite(aircraftClips.pre_seconds) && Number.isFinite(aircraftClips.post_seconds) &&
        aircraftClips.pre_seconds + aircraftClips.post_seconds > 10)
      errors.push('capture.aircraft_clips pre_seconds plus post_seconds cannot exceed 10 seconds');
    if (Number.isFinite(aircraftClips.minimum_slant_range_km) &&
        Number.isFinite(aircraftClips.maximum_slant_range_km) &&
        aircraftClips.minimum_slant_range_km > aircraftClips.maximum_slant_range_km)
      errors.push('capture.aircraft_clips minimum slant range cannot exceed maximum slant range');
    if (Number.isFinite(aircraftClips.minimum_elevation_deg) &&
        Number.isFinite(aircraftClips.maximum_elevation_deg) &&
        aircraftClips.minimum_elevation_deg > aircraftClips.maximum_elevation_deg)
      errors.push('capture.aircraft_clips minimum elevation cannot exceed maximum elevation');
  }
  string(device.type, 'capture.device.type');
  const profile = DEVICE_PROFILES.find(item => item.type === device.type);
  if (!profile) errors.push('capture.device.type is not supported');
  else {
    const allowed = new Set(Object.keys(profile.device));
    Object.keys(device).forEach(key => {
      // Keep a dormant Kraken record when switching receiver profiles.
      if (!allowed.has(key) && key !== 'array_geometry')
        errors.push(`capture.device.${key} is not valid for ${device.type}`);
    });
    allowed.forEach(key => {
      if (!Object.prototype.hasOwnProperty.call(device, key) &&
          !(device.type === 'RspDuo' && key === 'serial'))
        errors.push(`capture.device.${key} is required for ${device.type}`);
    });
  }
  const geometryErrors = recordErrors(device.array_geometry);
  errors.push(...geometryErrors);
  let arrayGeometry = null;
  if (device.type === 'Kraken' && device.array_geometry !== undefined && !geometryErrors.length) {
    const assessment = evaluateKrakenArray(root);
    arrayGeometry = assessment;
    advise('capture.device.array_geometry', assessment.valid ?
      assessment.worldBearingEligible ?
        'Bearing output eligible from operator-confirmed geometry; physical agreement remains unverified.' :
        'Layout record complete; physical agreement unverified. Bearing output is disabled or ineligible.' :
      'Layout record incomplete or needs review. Passive radar may still be configured; bearing is unavailable.');
  }
  boolean(replay.state, 'capture.replay.state');
  boolean(replay.loop, 'capture.replay.loop');
  string(replay.file, 'capture.replay.file');
  if (typeof replay.file === 'string' && !path.isAbsolute(replay.file))
    errors.push('capture.replay.file must be a full absolute path');
  if (pathPolicy?.kind === 'systemd-linux' && typeof replay.file === 'string' &&
      path.isAbsolute(replay.file) &&
      servicePathHidden(pathPolicy.hiddenReplayRoots, replay.file))
    errors.push('capture.replay.file is hidden by the packaged Linux service sandbox; use /var/lib/vectorwarp/recordings/ or another service-readable system path');
  for (const key of ['recording_sha256', 'acquisition_sha256'])
    if (replay[key] !== undefined && (typeof replay[key] !== 'string' || !/^[a-f0-9]{64}$/.test(replay[key])))
      errors.push(`capture.replay.${key} must be an externally trusted lowercase SHA-256`);
  if (replay.acquisition_sha256 !== undefined && replay.recording_sha256 === undefined)
    errors.push('capture.replay.acquisition_sha256 requires recording_sha256');
  const replaySurveillance = replay.surveillance_channels === undefined ? [] :
    integerArray(replay.surveillance_channels, 'capture.replay.surveillance_channels');
  const replayReferences = replay.reference_channels === undefined ? [] :
    integerArray(replay.reference_channels, 'capture.replay.reference_channels');
  if (Number.isInteger(replay.channel_count))
    [...replaySurveillance, ...replayReferences, replay.reference_channel]
      .filter(Number.isInteger).forEach(channel => {
        if (channel >= replay.channel_count)
          errors.push('capture.replay channel roles must be inside channel_count');
      });
  if (replay.reference_mode === 'dedicated' &&
      replay.reference_channels !== undefined &&
      (replayReferences.length !== 1 ||
       (replay.reference_channel !== undefined && replayReferences[0] !== replay.reference_channel)))
    errors.push('capture.replay dedicated reference_channel and reference_channels must identify the same single channel');
  if (new Set(replaySurveillance).size !== replaySurveillance.length)
    errors.push('capture.replay.surveillance_channels must be unique');
  if (new Set(replayReferences).size !== replayReferences.length)
    errors.push('capture.replay.reference_channels must be unique');
  if (replay.format === 'usrp-blocks' && replay.legacy_block_samples === undefined)
    errors.push('capture.replay.legacy_block_samples is required for usrp-blocks replay');
  if (residentSnapshot) {
    boolean(residentSnapshot.enabled, 'capture.replay.resident_snapshot.enabled');
    if (residentSnapshot.enabled === true) {
      string(residentSnapshot.output_directory,
        'capture.replay.resident_snapshot.output_directory');
      if (typeof residentSnapshot.output_directory === 'string' &&
          !path.isAbsolute(residentSnapshot.output_directory))
        errors.push('capture.replay.resident_snapshot.output_directory must be a full absolute path');
      number(residentSnapshot.max_frames, 'capture.replay.resident_snapshot.max_frames');
      number(residentSnapshot.max_bytes, 'capture.replay.resident_snapshot.max_bytes',
        {integer: true, min: 1, max: 268435456});
      for (const field of ['source_sha256', 'rf_profile_sha256']) {
        string(residentSnapshot[field], `capture.replay.resident_snapshot.${field}`);
        if (typeof residentSnapshot[field] === 'string' &&
            !/^[0-9a-fA-F]{64}$/.test(residentSnapshot[field]))
          errors.push(`capture.replay.resident_snapshot.${field} must contain exactly 64 hexadecimal characters`);
      }
      string(residentSnapshot.rf_profile_file,
        'capture.replay.resident_snapshot.rf_profile_file');
      if (typeof residentSnapshot.rf_profile_file === 'string' &&
          !path.isAbsolute(residentSnapshot.rf_profile_file))
        errors.push('capture.replay.resident_snapshot.rf_profile_file must be a full absolute path');
      if (replay.state !== true)
        errors.push('resident snapshot export is restricted to offline replay');
      if (!residentMode)
        errors.push('resident snapshot export requires auto or gpu-resident acceleration');
      if ((processConfig.surveillance_fusion?.mode ?? 'noncoherent') === 'noncoherent')
        errors.push('resident snapshot export requires an already selected coherent resident product route');
      if (multiIlluminator?.enabled === true)
        errors.push('resident snapshot export requires single-lane processing');
      if (processingCapabilities(root).referenceMode !== 'array_eigenbeam')
        errors.push('resident snapshot export currently requires array_eigenbeam reference synthesis');
    }
  }

  if (device.type === 'Kraken') {
    const inferredChannelCount = device.channel_count ??
      (Array.isArray(device.gain) ? device.gain.length : undefined);
    const channelCount = number(inferredChannelCount,
      device.channel_count === undefined ? 'capture.device.gain channel count' :
        'capture.device.channel_count', {integer: true, min: 2,
          max: KRAKEN_MAX_CHANNELS});
    if (!(replay.state === true && Number.isInteger(replay.channel_count))) {
      const referenceChannel = number(device.reference_channel,
        'capture.device.reference_channel', {integer: true, min: 0});
      const surveillance = integerArray(device.surveillance_channels,
        'capture.device.surveillance_channels');
      const synthesis = requireObject(processConfig.reference_synthesis,
        'process.reference_synthesis');
      if (!['dedicated', 'array_eigenbeam'].includes(synthesis.mode))
        errors.push('process.reference_synthesis.mode must be dedicated or array_eigenbeam');
      const referenceChannels = synthesis.channels === undefined ? [] :
        integerArray(synthesis.channels, 'process.reference_synthesis.channels');
      if (channelCount !== null) {
        if (referenceChannel !== null && referenceChannel >= channelCount)
          errors.push('capture.device.reference_channel is outside channel_count');
        [...surveillance, ...referenceChannels].forEach(channel => {
          if (Number.isInteger(channel) && channel >= channelCount)
            errors.push(`capture.device.channel_count: input ${channel} is outside the configured channel count`);
        });
        if (synthesis.mode === 'dedicated' &&
            surveillance.includes(referenceChannel))
          errors.push('capture.device.reference_channel cannot also be a surveillance channel in dedicated mode');
        if (synthesis.mode === 'array_eigenbeam' && referenceChannels.length === 1)
          errors.push('process.reference_synthesis.channels requires at least two reference channels in array_eigenbeam mode');
      }
    }
    if (capture.fs !== 2400000)
      errors.push('KrakenSDR Suite V2 requires capture.fs to be 2400000');
    if (capture.fc < 24000000 || capture.fc > 1766000000)
      advise('capture.fc', 'Outside the standard tuner range. Check upstream tuning.');
    const heimdall = requireObject(device.heimdall,
      'capture.device.heimdall');
    string(heimdall.host, 'capture.device.heimdall.host');
    number(heimdall.port, 'capture.device.heimdall.port');
    if (heimdall.control_port !== undefined) {
      number(heimdall.control_port, 'capture.device.heimdall.control_port',
        {integer: true, min: 2, max: 65535});
      if (heimdall.control_port === heimdall.port)
        errors.push('capture.device.heimdall.control_port must differ from the IQ data port');
    }
    if (heimdall.gain !== undefined && !(heimdall.gain === 'keep' || heimdall.gain === -1 ||
        (typeof heimdall.gain === 'number' && Number.isFinite(heimdall.gain) && heimdall.gain >= 0 && heimdall.gain <= 50 && Math.round(heimdall.gain * 10) === heimdall.gain * 10)))
      errors.push('capture.device.heimdall.gain must be keep, automatic (-1), or 0 through 50 dB in 0.1 dB steps');
    if (['measured', 'measured_per_channel'].includes(heimdall.gain_control) && heimdall.gain === -1)
      errors.push('capture.device.heimdall.gain must be keep or a fixed starting gain when automatic headroom selection is enabled; Suite AGC cannot provide a stable measurement.');
    if (heimdall.channel_gains !== undefined) {
      if (!Array.isArray(heimdall.channel_gains) || heimdall.channel_gains.length !== channelCount)
        errors.push('capture.device.heimdall.channel_gains must contain exactly one value per configured channel');
      else heimdall.channel_gains.forEach((value, index) => {
        if (!(typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 50 &&
            Math.round(value * 10) === value * 10))
          errors.push(`capture.device.heimdall.channel_gains[${index}] must be 0 through 50 dB in 0.1 dB steps`);
      });
      if (heimdall.gain_control === 'measured')
        errors.push('capture.device.heimdall.channel_gains requires fixed gain adjustment; common automatic gain cannot preserve a channel vector');
    }
    if (typeof heimdall.host === 'string' &&
        !net.isIP(heimdall.host) && !/^(?=.{1,253}$)[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?$/i.test(heimdall.host))
      errors.push('capture.device.heimdall.host must be an IP address or hostname, without a URL or port');
  } else if (device.type === 'Usrp') {
    string(device.address, 'capture.device.address');
    if (typeof device.address === 'string' && device.address.trim()) {
      const arguments_ = device.address.split(',');
      const keys = arguments_.map(item => /^([A-Za-z0-9_]+)\s*=/.exec(item.trim())?.[1]);
      if (/[\u0000-\u001f\u007f]/.test(device.address) ||
          !arguments_.every(item =>
            /^[A-Za-z0-9_]+\s*=\s*[^,=\s](?:[^,=]*[^,=\s])?$/.test(item.trim())) ||
          keys.some((key, index) => key && keys.indexOf(key) !== index))
        errors.push('capture.device.address must use unique UHD key=value arguments, such as type=b200 or type=b200,serial=…');
    }
    string(device.subdev, 'capture.device.subdev');
    if (device.timing_source !== undefined && !['internal', 'gpsdo'].includes(device.timing_source))
      errors.push('capture.device.timing_source must be internal or gpsdo');
    if (!Array.isArray(device.antenna) || device.antenna.length !== 2)
      errors.push('capture.device.antenna must contain exactly two values');
    if (!Array.isArray(device.gain) || device.gain.length !== 2)
      errors.push('capture.device.gain must contain exactly two values');
    else device.gain.forEach((value, index) =>
      number(value, `capture.device.gain[${index}]`));
    if (Array.isArray(device.antenna)) device.antenna.forEach((value, index) =>
      string(value, `capture.device.antenna[${index}]`));
  } else if (device.type === 'HackRF') {
    if (capture.fs < 2000000 || capture.fs > 20000000)
      errors.push('capture.fs must be between 2 and 20 MS/s for HackRF');
    if (capture.fc < 1000000)
      errors.push('capture.fc must be at least 1 MHz for HackRF');
    ['serial', 'gain_lna', 'gain_vga', 'amp_enable'].forEach(key => {
      if (!Array.isArray(device[key]) || device[key].length !== 2)
        errors.push(`capture.device.${key} must contain exactly two values`);
    });
    if (Array.isArray(device.serial)) {
      device.serial.forEach((value, index) => {
        string(value, `capture.device.serial[${index}]`);
        if (typeof value !== 'string' || !/^[a-f0-9]{1,32}$/i.test(value))
          errors.push(`capture.device.serial[${index}] must be the device's hexadecimal serial number (up to 32 characters)`);
      });
      if (device.serial[0]?.toLowerCase?.() === device.serial[1]?.toLowerCase?.())
        errors.push('capture.device.serial must identify two different HackRF receivers');
      else if (device.serial.length === 2 && device.serial.every(value =>
          typeof value === 'string' && /^[a-f0-9]{1,32}$/i.test(value)) &&
          (device.serial[0].endsWith(device.serial[1]) ||
           device.serial[1].endsWith(device.serial[0])))
        errors.push('capture.device.serial values overlap as HackRF serial suffixes; use distinct full serials');
    }
    if (Array.isArray(device.gain_lna))
      device.gain_lna.forEach((value, index) => {
        number(value, `capture.device.gain_lna[${index}]`,
          {integer: true, min: 0, max: 40});
        if (Number.isInteger(value) && value % 8 !== 0)
          errors.push(`capture.device.gain_lna[${index}] must use an 8 dB step`);
      });
    if (Array.isArray(device.gain_vga))
      device.gain_vga.forEach((value, index) => {
        number(value, `capture.device.gain_vga[${index}]`,
          {integer: true, min: 0, max: 62});
        if (Number.isInteger(value) && value % 2 !== 0)
          errors.push(`capture.device.gain_vga[${index}] must use a 2 dB step`);
      });
    if (Array.isArray(device.amp_enable))
      device.amp_enable.forEach((value, index) =>
        boolean(value, `capture.device.amp_enable[${index}]`));
  } else if (device.type === 'RspDuo') {
    if (device.serial !== undefined && (typeof device.serial !== 'string' || device.serial.length > 160 || /[\u0000-\u001f\u007f]/.test(device.serial)))
      errors.push('capture.device.serial must be an optional RSPduo serial string');
    number(capture.fc, 'capture.fc', {integer: true, min: 1,
      max: 2000000000});
    oneOf(capture.fs, 'capture.fs',
      [62500, 125000, 250000, 500000, 1000000, 2000000]);
    number(device.agcSetPoint, 'capture.device.agcSetPoint');
    number(device.bandwidthNumber, 'capture.device.bandwidthNumber',
      {integer: true, min: 0});
    oneOf(device.bandwidthNumber, 'capture.device.bandwidthNumber');
    if (!Array.isArray(device.gainReduction) ||
        device.gainReduction.length !== 2)
      errors.push('capture.device.gainReduction must contain exactly two values');
    else device.gainReduction.forEach((value, index) =>
      number(value, `capture.device.gainReduction[${index}]`,
        {integer: true, min: 20, max: 59}));
    const lnaBand = FIELD_RULES['capture.device.lnaState'].frequencyBands
      .find(band => capture.fc < band.belowHz);
    number(device.lnaState, 'capture.device.lnaState',
      {integer: true, min: 0, max: lnaBand?.max ?? 8});
    boolean(device.dabNotch, 'capture.device.dabNotch');
    boolean(device.rfNotch, 'capture.device.rfNotch');
  }

  const cpi = number(data.cpi, 'process.data.cpi');
  const buffer = number(data.buffer, 'process.data.buffer');
  // DSP/channelization follow the recording while replay is active. Keep
  // the saved live receiver profile independent of the recorded passband.
  const sampleRate = replay.state === true && Number.isFinite(replay.sample_rate_hz) ?
    replay.sample_rate_hz : Number.isFinite(capture.fs) ? capture.fs : null;
  const inputFrequency = replay.state === true && Number.isFinite(replay.frequency_hz) ?
    replay.frequency_hz : capture.fc;
  const topologyInputCount = replay.state === true && Number.isInteger(replay.channel_count) ?
    replay.channel_count : device.type === 'Kraken' && Number.isInteger(device.channel_count) ?
      device.channel_count : 2;
  if (processConfig.delay_reference_sample_rate_hz !== undefined) try {
    const effective = effectiveDelayConfig(root, sampleRate,
      {scaleNeighborhoods: multiIlluminator?.enabled !== true}).process;
    ambiguity = effective.ambiguity; clutter = effective.clutter; detection = effective.detection;
  } catch (error) { errors.push(`process.delay_reference_sample_rate_hz: ${error.message}`); }
  if (multiIlluminator) {
    boolean(multiIlluminator.enabled, 'process.multi_illuminator.enabled');
    if (multiIlluminator.enabled === true &&
        multiIlluminator.input_phase_correction === 'automatic')
      errors.push('process.multi_illuminator.input_phase_correction automatic is not available in this processor build');
    const siteGrouping = multiIlluminator.transmitter_site_grouping ?? 'automatic';
    oneOf(siteGrouping, 'process.multi_illuminator.transmitter_site_grouping');
    const illuminators = multiIlluminator.illuminators;
    if (!Array.isArray(illuminators) || illuminators.length > 16 ||
        (multiIlluminator.enabled === true && illuminators.length < 1))
      errors.push('process.multi_illuminator.illuminators must contain 1 to 16 entries when enabled');
    else {
      const ids = new Set(), frequencies = new Set();
      let enabled = 0;
      illuminators.forEach((illuminator, index) => {
        const field = `process.multi_illuminator.illuminators[${index}]`;
        if (!isObject(illuminator)) {
          errors.push(`${field} must be an object`);
          return;
        }
        const allowed = new Set(['id', 'enabled', 'standard', 'frequency_hz',
          'occupied_bandwidth_hz', 'output_sample_rate_hz',
          'filter_taps_per_phase', 'atsc_reference_mode', 'input_role_mode', 'reference_channel',
          'surveillance_channels', 'transmitter', 'tracker_min_snr_db',
          'transmitter_location', 'detection_manifold_file', 'bearing_manifold_file']);
        Object.keys(illuminator).forEach(key => {
          if (!allowed.has(key)) errors.push(`${field}.${key} is not supported`);
        });
        string(illuminator.id, `${field}.id`);
        if (typeof illuminator.id === 'string' &&
            !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(illuminator.id))
          errors.push(`${field}.id must use 1 to 64 letters, numbers, dots, underscores, or dashes`);
        if (ids.has(illuminator.id))
          errors.push('process.multi_illuminator illuminator IDs must be unique');
        ids.add(illuminator.id);
        boolean(illuminator.enabled, `${field}.enabled`);
        if (illuminator.transmitter_location !== undefined) {
          const site = requireObject(illuminator.transmitter_location, `${field}.transmitter_location`);
          number(site.latitude, `${field}.transmitter_location.latitude`, {min: -90, max: 90});
          number(site.longitude, `${field}.transmitter_location.longitude`, {min: -180, max: 180});
          number(site.altitude, `${field}.transmitter_location.altitude`);
        }
        for (const key of ['detection_manifold_file', 'bearing_manifold_file'])
          if (illuminator[key] !== undefined &&
              (typeof illuminator[key] !== 'string' || !illuminator[key].trim()))
            errors.push(`${field}.${key} must be a non-empty path`);
        if (illuminator.detection_manifold_file !== undefined || illuminator.bearing_manifold_file !== undefined) {
          if ((processConfig.surveillance_fusion?.mode ?? 'noncoherent') === 'noncoherent')
            errors.push(`${field} manifold files require coherent geometry`);
          if (processConfig.bearing?.reference_anchor === 'fixed_site_projected')
            errors.push('fixed_site_projected requires ideal lane-bound geometry, no measured manifold');
        }
        if (illuminator.enabled === true) enabled += 1;
        if (illuminator.tracker_min_snr_db !== undefined)
          number(illuminator.tracker_min_snr_db, `${field}.tracker_min_snr_db`,
            {min: -100, max: 100});
        oneOf(illuminator.standard, `${field}.standard`,
          ['AM', 'FM', 'DAB', 'DRM', 'ATSC', 'ATSC-1.0', 'ATSC-3.0',
            'DVB-T/T2', 'ISDB-T', 'DTMB', 'CUSTOM']);
        if (illuminator.atsc_reference_mode !== undefined) {
          oneOf(illuminator.atsc_reference_mode, `${field}.atsc_reference_mode`,
            ['automatic', 'raw', 'pilot_aided']);
          if (!['ATSC', 'ATSC-1.0', 'ATSC-3.0'].includes(illuminator.standard) &&
              illuminator.atsc_reference_mode !== 'raw')
            errors.push(`${field}.atsc_reference_mode requires an ATSC standard unless it is raw`);
          if (illuminator.standard === 'ATSC-3.0' &&
              illuminator.atsc_reference_mode === 'pilot_aided')
            errors.push(`${field}.atsc_reference_mode pilot_aided requires standard ATSC 1.0`);
          if (illuminator.atsc_reference_mode === 'pilot_aided') {
            if (illuminator.reference_channel === undefined &&
                processingCapabilities(root).referenceMode === 'array_eigenbeam')
              errors.push(`${field}.atsc_reference_mode pilot_aided does not support array reference`);
          }
        }
        const laneReference = illuminator.reference_channel === undefined ? null :
          number(illuminator.reference_channel, `${field}.reference_channel`,
            {integer: true, min: 0, max: 7});
        const laneSurveillance = illuminator.surveillance_channels === undefined ? [] :
          integerArray(illuminator.surveillance_channels,
            `${field}.surveillance_channels`);
        const explicitInputRoles = laneReference !== null || laneSurveillance.length > 0;
        // Missing legacy values inherit the receiver topology. Automatic
        // per-lane qualification is rejected until the native selector exists.
        const inputRoleMode = illuminator.input_role_mode ?? 'manual';
        oneOf(inputRoleMode, `${field}.input_role_mode`, ['automatic', 'manual']);
        if (inputRoleMode === 'automatic' && explicitInputRoles)
          errors.push(`${field} automatic input roles cannot include manual channel overrides`);
        if (multiIlluminator.enabled === true && illuminator.enabled === true &&
            inputRoleMode === 'automatic')
          errors.push(`${field}.input_role_mode automatic is not available in this processor build`);
        if (inputRoleMode === 'manual' && laneReference !== null &&
            laneSurveillance.length === 0)
          errors.push(`${field}.surveillance_channels is required with a per-lane reference channel`);
        if (laneReference === null && laneSurveillance.length > 0)
          errors.push(`${field}.reference_channel is required with per-lane surveillance channels`);
        if (laneReference !== null && laneReference >= topologyInputCount)
          errors.push(`${field}.reference_channel must identify one of the ${topologyInputCount} coherent inputs`);
        laneSurveillance.forEach((channel, channelIndex) => {
          if (channel >= topologyInputCount)
            errors.push(`${field}.surveillance_channels[${channelIndex}] must identify one of the ${topologyInputCount} coherent inputs`);
        });
        if (laneReference !== null && laneSurveillance.includes(laneReference))
          errors.push(`${field}.reference_channel cannot also be a surveillance channel`);
        if (new Set(laneSurveillance).size !== laneSurveillance.length)
          errors.push(`${field}.surveillance_channels must be unique`);
        const frequency = number(illuminator.frequency_hz,
          `${field}.frequency_hz`, {integer: true, min: 1, max: UINT32_MAX});
        const bandwidth = number(illuminator.occupied_bandwidth_hz,
          `${field}.occupied_bandwidth_hz`, {integer: true, min: 1, max: UINT32_MAX});
        const outputRate = number(illuminator.output_sample_rate_hz,
          `${field}.output_sample_rate_hz`, {integer: true, min: 1, max: UINT32_MAX});
        const activeLane = multiIlluminator.enabled === true &&
          illuminator.enabled === true;
        if (frequencies.has(frequency))
          errors.push('process.multi_illuminator illuminator frequencies must be unique');
        frequencies.add(frequency);
        if (activeLane && Number.isInteger(sampleRate) && Number.isInteger(outputRate) &&
            sampleRate % outputRate !== 0)
          errors.push(`${field}.output_sample_rate_hz must divide ${replay.state === true ?
            'the replay sample rate' : 'capture.fs'} exactly`);
        if (activeLane && Number.isFinite(bandwidth) && Number.isFinite(outputRate) &&
            bandwidth >= outputRate)
          errors.push(`${field}.occupied_bandwidth_hz must be smaller than output_sample_rate_hz`);
        if (activeLane && Number.isFinite(frequency) && Number.isFinite(bandwidth) &&
            Number.isFinite(inputFrequency) && Number.isFinite(sampleRate) &&
            Math.abs(frequency - inputFrequency) + bandwidth / 2 >= sampleRate / 2)
          errors.push(`${field}.frequency_hz and occupied bandwidth must fit inside the captured passband`);
        if (illuminator.filter_taps_per_phase !== undefined)
          number(illuminator.filter_taps_per_phase,
            `${field}.filter_taps_per_phase`, {integer: true, min: 4, max: 128});
        const tapsPerPhase = illuminator.filter_taps_per_phase ?? 24;
        if (activeLane && illuminator.standard === 'ATSC-3.0' && bandwidth === 5380000)
          advise(`${field}.occupied_bandwidth_hz`,
            '5.38 MHz is the ATSC 1.0 width and trims useful carriers from an unreduced 6 MHz ATSC 3.0 waveform; use the decoded profile width (5.832844 MHz for bsr=2, C_red=0).');
        if (activeLane && illuminator.standard === 'ATSC-3.0' &&
            bandwidth >= 5800000 && tapsPerPhase < 96)
          advise(`${field}.filter_taps_per_phase`,
            'A short channel filter attenuates the outer ATSC 3.0 carriers; use at least 96 taps per phase after checking the live CPI budget.');
        if (activeLane && ['ATSC', 'ATSC-1.0'].includes(illuminator.standard) &&
            bandwidth === 5380000 && tapsPerPhase < 48)
          advise(`${field}.filter_taps_per_phase`,
            'A short channel filter attenuates the ATSC 1.0 occupied-band edge; 48 to 64 taps per phase preserves it more accurately.');
        if (activeLane && Number.isInteger(sampleRate) && Number.isInteger(outputRate) &&
            outputRate > 0 && sampleRate % outputRate === 0 &&
            tapsPerPhase * (sampleRate / outputRate) + 1 > 131071)
          errors.push(`${field} full-quality channel filter exceeds 131,071 taps`);
        const transmitter = requireObject(illuminator.transmitter,
          `${field}.transmitter`);
        string(transmitter.name, `${field}.transmitter.name`);
        number(transmitter.latitude, `${field}.transmitter.latitude`, {min: -90, max: 90});
        number(transmitter.longitude, `${field}.transmitter.longitude`, {min: -180, max: 180});
        number(transmitter.altitude, `${field}.transmitter.altitude`,
          {min: -500, max: 100000});
        Object.keys(transmitter).forEach(key => {
          if (!['name', 'site_id', 'latitude', 'longitude', 'altitude'].includes(key))
            errors.push(`${field}.transmitter.${key} is not supported`);
        });
        if (transmitter.site_id !== undefined) {
          string(transmitter.site_id, `${field}.transmitter.site_id`);
          if (typeof transmitter.site_id === 'string' &&
              !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(transmitter.site_id))
            errors.push(`${field}.transmitter.site_id must use 1 to 64 letters, numbers, dots, underscores, or dashes`);
        }
        if (siteGrouping === 'explicit' && illuminator.enabled === true &&
            !(typeof transmitter.site_id === 'string' && transmitter.site_id.trim()))
          errors.push(`${field}.transmitter.site_id is required by explicit transmitter-site grouping`);
      });
      if (multiIlluminator.enabled === true && enabled < 1)
        errors.push('process.multi_illuminator requires at least one enabled illuminator');
    }
    if (multiIlluminator.enabled === true)
      number(ports.illuminator, 'network.ports.illuminator');
  }
  const cpiSamples = cpi !== null && sampleRate !== null ?
    Math.round(cpi * sampleRate) : null;
  if (cpiSamples !== null && (cpiSamples < 2000 || cpiSamples > INT32_MAX))
    errors.push('process.data.cpi must produce 2,000–2,147,483,647 samples for the spectrum and FFT processors');
  if (cpi !== null && buffer !== null && sampleRate !== null &&
      cpi * buffer * sampleRate > UINT32_MAX)
    errors.push('process.data.buffer creates an unsupported capture buffer');

  const delayMin = number(ambiguity.delayMin, 'process.ambiguity.delayMin');
  const delayMax = number(ambiguity.delayMax, 'process.ambiguity.delayMax');
  const dopplerMin = number(ambiguity.dopplerMin, 'process.ambiguity.dopplerMin');
  const dopplerMax = number(ambiguity.dopplerMax, 'process.ambiguity.dopplerMax');
  if (delayMin !== null && delayMax !== null && delayMin >= delayMax)
    errors.push('process.ambiguity.delayMin must be smaller than process.ambiguity.delayMax');
  if (dopplerMin !== null && dopplerMax !== null && dopplerMin >= dopplerMax)
    errors.push('process.ambiguity.dopplerMin must be smaller than process.ambiguity.dopplerMax');
  const delayBins = delayMin !== null && delayMax !== null ?
    delayMax - delayMin + 1 : null;
  if (delayBins !== null && (delayBins < 2 || delayBins > 65534))
    errors.push('process.ambiguity.delayMax: delay span must contain 2–65534 bins');
  if (delayMin !== null && delayMin > 1)
    errors.push('process.ambiguity.delayMin must be 1 or smaller');
  if (delayMax !== null && delayMax < -1)
    errors.push('process.ambiguity.delayMax must be -1 or larger');
  let ambiguityCpi = null, ambiguityDopplerBins = null;
  if (cpi !== null && cpiSamples !== null && cpiSamples > 0 &&
      dopplerMin !== null && dopplerMax !== null && dopplerMin < dopplerMax) {
    const middle = (dopplerMin + dopplerMax) / 2;
    if (dopplerMin + dopplerMax < INT32_MIN || dopplerMin + dopplerMax > INT32_MAX)
      errors.push('process.ambiguity.dopplerMax: combined Doppler limits overflow the processor');
    const positiveSteps = Math.max(0,
      Math.floor((dopplerMax - middle) * (cpiSamples / sampleRate)));
    const dopplerBins = 1 + 2 * positiveSteps;
    ambiguityDopplerBins = dopplerBins;
    if (dopplerBins > cpiSamples || dopplerBins > 65535)
      errors.push('process.ambiguity.dopplerMax: Doppler span creates too many bins for this CPI');
    else {
      const correlationSamples = Math.floor(cpiSamples / dopplerBins);
      if (delayMin !== null && delayMax !== null &&
          (delayMin <= -correlationSamples || delayMax >= correlationSamples))
        errors.push(`process.ambiguity.delayMax: delay limits must be within ${1 - correlationSamples} to ${correlationSamples - 1} samples; reduce the delay range or narrow the Doppler span`);
      ambiguityCpi = correlationSamples * dopplerBins / sampleRate;
      // FFT rounding matches next_hamming (factors 2, 3 and 5). Never loop
      // through user-sized ranges: only bounded candidate lengths are searched.
      const required = correlationSamples + Math.max(Math.abs(delayMin || 0), Math.abs(delayMax || 0));
      let fft = Infinity;
      for (let a = 1; a <= 65535; a *= 2)
        for (let b = a; b <= 65535; b *= 3)
          for (let n = b; n <= 65535; n *= 5)
            if (n >= required) fft = Math.min(fft, n);
      if (!Number.isFinite(fft))
        errors.push('process.ambiguity.dopplerMax: widen the Doppler span; correlation FFT exceeds the processor limit');
      if (delayBins !== null && delayBins >= fft)
        errors.push('process.ambiguity.delayMax: reduce the delay span or narrow the Doppler span');
    }
  }
  boolean(clutter.enable, 'process.clutter.enable');
  const followsAmbiguity = clutter.followAmbiguity === undefined ? false :
    (boolean(clutter.followAmbiguity, 'process.clutter.followAmbiguity'),
      clutter.followAmbiguity);
  // Existing numeric configurations retain their manual half-open interval.
  // The opt-in default derives [ambiguity.delayMin, ambiguity.delayMax + 1),
  // covering every inclusive ambiguity-map bin without site-specific tuning.
  const configuredClutterMin = clutter.delayMin === undefined ? null : number(clutter.delayMin, 'process.clutter.delayMin');
  const configuredClutterMax = clutter.delayMax === undefined ? null : number(clutter.delayMax, 'process.clutter.delayMax');
  if (!followsAmbiguity && (configuredClutterMin === null || configuredClutterMax === null))
    errors.push('process.clutter: provide delayMin and delayMax, or enable followAmbiguity');
  if (followsAmbiguity && delayMax === INT32_MAX)
    errors.push('process.clutter.followAmbiguity: ambiguity.delayMax must leave room for the exclusive filter endpoint');
  const clutterMin = followsAmbiguity ? delayMin : configuredClutterMin;
  const clutterMax = followsAmbiguity && delayMax !== null && delayMax < INT32_MAX ?
    delayMax + 1 : (followsAmbiguity ? null : configuredClutterMax);
  if (clutterMin !== null && clutterMax !== null && clutterMin >= clutterMax)
    errors.push('process.clutter.delayMin must be smaller than process.clutter.delayMax');
  if (clutterMin !== null && clutterMax !== null && cpiSamples !== null &&
      clutterMax - clutterMin > cpiSamples)
    errors.push('process.clutter.delayMax: filter delay span must fit inside the CPI sample count');
  if (clutterMin !== null && clutterMax !== null && cpiSamples !== null &&
      clutterMax - clutterMin + cpiSamples + 1 > INT32_MAX)
    errors.push('process.clutter.delayMax: filter FFT exceeds the processor limit');
  // Once the physical receiver is bypassed, validate processing against the
  // recording's channel topology rather than the machine's installed SDR.
  const capabilities = processingCapabilities(root);
  const experimental = experimentalSettingSupport(root, capabilities);
  if (!capabilities.validTopology)
    errors.push('capture channel roles must be unique, in range, and identify one disjoint dedicated reference or a valid array reference');
  const processingDevice = {...device, channel_count: capabilities.channelCount,
    reference_channel: capabilities.referenceChannel,
    surveillance_channels: capabilities.surveillanceChannels};
  const channels = capabilities.channelCount;
  const paths = capabilities.surveillanceChannels.length;
  if (capabilities.referenceMode === 'array_eigenbeam' &&
      !capabilities.coherent)
    errors.push('array_eigenbeam reference synthesis requires a coherent multi-channel capture or qualified replay');
  // Lower-bound estimate only: FFT plans, allocator overhead and other services
  // need additional memory. Deployment-specific budgets may be set by the host.
  const estimatedBytes = 16 * (channels * cpiSamples * buffer +
    (channels + paths + 1) * cpiSamples +
    paths * (9 * cpiSamples + (clutterMax - clutterMin) ** 2));
  const memoryBudget = Number(process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB || 0) * 1048576;
  if (memoryBudget > 0 && estimatedBytes > memoryBudget)
    errors.push('process.data.cpi: estimated radar memory exceeds this installation’s configured budget');
  boolean(detection.enable, 'process.detection.enable');
  number(detection.pfa, 'process.detection.pfa');
  ['nGuard', 'nTrain'].forEach(key =>
    number(detection[key], `process.detection.${key}`));
  number(detection.nCentroid, 'process.detection.nCentroid');
  number(detection.minDelay, 'process.detection.minDelay');
  number(detection.minDoppler, 'process.detection.minDoppler');
  let trackBeforeDetect = null;
  if (detection.track_before_detect !== undefined) {
    trackBeforeDetect = requireObject(detection.track_before_detect,
      'process.detection.track_before_detect');
    boolean(trackBeforeDetect.enable,
      'process.detection.track_before_detect.enable');
    if (trackBeforeDetect.fractional_delay !== undefined)
      boolean(trackBeforeDetect.fractional_delay,
        'process.detection.track_before_detect.fractional_delay');
    if (trackBeforeDetect.fractional_delay === true && trackBeforeDetect.enable !== true)
      errors.push('process.detection.track_before_detect.fractional_delay requires enabled track-before-detect');
    number(trackBeforeDetect.max_cpis, 'process.detection.track_before_detect.max_cpis');
    if (Number.isInteger(trackBeforeDetect.max_cpis) &&
        ![4, 8].includes(trackBeforeDetect.max_cpis))
      errors.push('process.detection.track_before_detect.max_cpis must be 4 or 8');
    number(trackBeforeDetect.motion_bins, 'process.detection.track_before_detect.motion_bins');
  }
  if (delayBins !== null && Number.isInteger(detection.nGuard) &&
      Number.isInteger(detection.nTrain) &&
      2 * detection.nGuard + 2 >= delayBins)
    errors.push('process.detection.nGuard: reduce guard cells or widen the delay span so noise cells remain');
  if (delayMin !== null && delayMax !== null &&
      Number.isInteger(detection.minDelay) &&
      (detection.minDelay < delayMin || detection.minDelay > delayMax))
    errors.push('process.detection.minDelay must be inside the ambiguity delay span');
  if (dopplerMin !== null && dopplerMax !== null &&
      Number.isFinite(detection.minDoppler) &&
      detection.minDoppler > Math.max(Math.abs(dopplerMin), Math.abs(dopplerMax)))
    errors.push('process.detection.minDoppler must be inside the Doppler span');
  boolean(tracker.enable, 'process.tracker.enable');
  if (tracker.enable && !detection.enable)
    errors.push('tracking requires process.detection.enable');
  ['M', 'N'].forEach(key => number(initiate[key],
    `process.tracker.initiate.${key}`));
  if (Number.isInteger(initiate.M) && Number.isInteger(initiate.N) &&
      initiate.M > initiate.N)
    errors.push('process.tracker.initiate.M cannot exceed process.tracker.initiate.N');
  const maxAcc = number(initiate.maxAcc, 'process.tracker.initiate.maxAcc');
  let stationaryClutter = null;
  if (initiate.stationary_clutter !== undefined) {
    stationaryClutter = requireObject(initiate.stationary_clutter,
      'process.tracker.initiate.stationary_clutter');
    boolean(stationaryClutter.enable,
      'process.tracker.initiate.stationary_clutter.enable');
    if (stationaryClutter.enable === true &&
        !Number.isInteger(stationaryClutter.persistence_cpis))
      errors.push('enabled persistent clutter filtering requires persistence_cpis');
  }
  if (initiate.doppler_ridge_clutter !== undefined) {
    const ridge = requireObject(initiate.doppler_ridge_clutter,
      'process.tracker.initiate.doppler_ridge_clutter');
    boolean(ridge.enable,
      'process.tracker.initiate.doppler_ridge_clutter.enable');
    number(ridge.persistence_cpis, 'process.tracker.initiate.doppler_ridge_clutter.persistence_cpis');
    number(ridge.doppler_tolerance_hz, 'process.tracker.initiate.doppler_ridge_clutter.doppler_tolerance_hz');
    number(ridge.minimum_delay_separation_km,
      'process.tracker.initiate.doppler_ridge_clutter.minimum_delay_separation_km');
  }
  if (maxAcc !== null && ambiguityCpi &&
      maxAcc * ambiguityCpi * ambiguityCpi > 65535)
    errors.push('process.tracker.initiate.maxAcc is too large for this CPI');
  number(tracker.delete, 'process.tracker.delete');
  errors.push(...temporalConfirmationErrors(config));
  {
    // Omitting the optional performance block selects native AUTO, not an
    // exemption from validation of resident-only tracker features.
    const performance = processConfig.performance === undefined ? {} :
      requireObject(processConfig.performance, 'process.performance');
    const {residentProducts} = experimental;
    if (initiate.doppler_ridge_clutter?.enable === true) {
      if (residentProducts)
        errors.push('Doppler ridge birth filter is unavailable in resident products');
      if (detection.enable !== true || tracker.enable !== true)
        errors.push('Doppler ridge birth filtering requires enabled detection and tracking');
    }
    if (residentMode) {
      if (tracker.enable && tracker.gpu_capacity === undefined &&
          (processConfig.surveillance_fusion?.mode ?? 'noncoherent') !== 'noncoherent')
        errors.push('process.tracker.gpu_capacity is required for gpu-resident tracking');
      if (!clutter.enable || !residentReferenceCompatible(capabilities))
        errors.push('gpu-resident requires clutter and a valid array or dedicated-reference capture topology');
    }
    if (stationaryClutter?.enable && tracker.enable !== true)
      errors.push('persistent stationary-clutter initiation filtering requires enabled tracking');
    if (performance.surveillance_workers !== undefined) number(performance.surveillance_workers,
      'process.performance.surveillance_workers');
    if (performance.fft_threads !== undefined) number(performance.fft_threads, 'process.performance.fft_threads');
  }
  if (processConfig.reference_synthesis !== undefined) {
    const synthesis = requireObject(processConfig.reference_synthesis,
      'process.reference_synthesis');
    number(synthesis.analysis_samples, 'process.reference_synthesis.analysis_samples');
    ['analysis_interval', 'power_iterations'].forEach(key =>
      number(synthesis[key], `process.reference_synthesis.${key}`));
    number(synthesis.covariance_smoothing, 'process.reference_synthesis.covariance_smoothing');
    number(synthesis.diagonal_loading, 'process.reference_synthesis.diagonal_loading');
    if (synthesis.power_iterations > 100)
      advise('process.reference_synthesis.power_iterations', 'May slow updates. Try 12 iterations.');
    if (synthesis.target_protection_enabled === true) {
      if (!experimental.targetProtection)
        errors.push('Experimental target protection requires single-lane coherent-beam-bank processing with confirmed array geometry and explicit CPU or portable GPU acceleration');
      if (!Number.isInteger(synthesis.target_protection_beams) ||
          !Number.isInteger(synthesis.target_protection_beam_index) ||
          synthesis.target_protection_beam_index >= synthesis.target_protection_beams)
        errors.push('Experimental target-protection beam index must be within its configured bank');
    }
  }
  if (root.save?.complex_cutout?.state === true) {
    if (!experimental.complexCutout)
      errors.push('Experimental complex cutout requires replay with explicit CPU or portable GPU acceleration');
    if (typeof root.save.complex_cutout.path !== 'string' ||
        !path.isAbsolute(root.save.complex_cutout.path))
      errors.push('Experimental complex cutout requires an absolute output path');
  }
  if (processConfig.surveillance_fusion !== undefined) {
    const fusion = requireObject(processConfig.surveillance_fusion,
      'process.surveillance_fusion');
    if (!['noncoherent', 'coherent_beam_bank', 'noise_whitened_beam_bank'].includes(fusion.mode))
      errors.push('process.surveillance_fusion.mode is not supported');
    if (fusion.effective_beam_trials !== undefined) {
      const configuredBeams = fusion.beams ?? 12;
      number(fusion.effective_beam_trials, 'process.surveillance_fusion.effective_beam_trials');
      if (Number.isFinite(configuredBeams) &&
          fusion.effective_beam_trials > configuredBeams)
        errors.push('effective_beam_trials cannot exceed surveillance_fusion.beams');
      if (fusion.mode === 'noncoherent' && Number.isFinite(configuredBeams) &&
          fusion.effective_beam_trials !== configuredBeams)
        errors.push('noncoherent fusion does not support reduced effective beam trials');
    }
    if (fusion.manifold_file !== undefined &&
        (typeof fusion.manifold_file !== 'string' || !fusion.manifold_file.trim()))
      errors.push('process.surveillance_fusion.manifold_file must be a non-empty path');
    if (fusion.manifold_file !== undefined &&
        !['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(fusion.mode))
      errors.push('process.surveillance_fusion.manifold_file requires coherent surveillance fusion');
    if (fusion.manifold_file !== undefined) {
      const heimdall=processingDevice.heimdall||{},geometry=processingDevice.array_geometry||{};
      const commonFixed=typeof heimdall.gain==='number'&&Number.isFinite(heimdall.gain)&&heimdall.gain>=0;
      const vectorFixed=Array.isArray(heimdall.channel_gains)&&
        heimdall.channel_gains.length===processingDevice.channel_count&&
        heimdall.channel_gains.every(value=>typeof value==='number'&&Number.isFinite(value)&&value>=0&&value<=50);
      if(heimdall.gain_control!==undefined&&heimdall.gain_control!=='fixed')
        errors.push('measured surveillance manifold requires capture.device.heimdall.gain_control: fixed');
      if(!commonFixed&&!vectorFixed)
        errors.push('measured surveillance manifold requires an explicit fixed requested gain vector');
      if(geometry.mapping_confirmed!==true||geometry.geometry_confirmed!==true||
         geometry.orientation_confirmed!==true)
        errors.push('measured surveillance manifold requires confirmed channel mapping, geometry, and orientation');
    }
    if (fusion.mode === 'coherent_beam_bank' || fusion.mode === 'noise_whitened_beam_bank') {
      const label = fusion.mode;
      if (!capabilities.coherentSurveillance)
        errors.push(`${label} surveillance fusion requires at least two coherent surveillance channels with a matching array reference`);
      if (!capabilities.surveillanceGeometry)
        errors.push(`${label} surveillance fusion requires complete nonzero array geometry and orientation`);
      if (!capabilities.confirmedGeometry)
        errors.push(`${label} surveillance fusion requires confirmed channel mapping, geometry, and orientation`);
      if (!isObject(processingDevice.array_geometry) || !Array.isArray(processingDevice.array_geometry.elements))
        errors.push(`${label} surveillance fusion requires capture.device.array_geometry.elements`);
      const geometryChannels = new Set((processingDevice.array_geometry?.elements || [])
        .filter(isObject).filter(element => Array.isArray(element.position) &&
          element.position.length === 3 && element.position.every(Number.isFinite))
        .map(element => element.daq_channel));
      if ((processingDevice.surveillance_channels || []).some(channel => !geometryChannels.has(channel)))
        errors.push(`${label} geometry must include every surveillance channel`);
      if (residentMode &&
          processingDevice.array_geometry?.shape !== 'regular_polygon')
        errors.push(`gpu-resident ${label} fusion requires regular_polygon geometry`);
      if (residentMode &&
          processingDevice.array_geometry?.shape === 'regular_polygon' &&
          !regularSurveillancePolygon(processingDevice.array_geometry, processingDevice.surveillance_channels))
        errors.push(`gpu-resident ${label} geometry must be an exact planar uniformly stepped regular polygon`);
      if (fusion.mode === 'noise_whitened_beam_bank' &&
          !residentMode)
        errors.push('noise_whitened_beam_bank fusion requires auto or gpu-resident acceleration');
      const reference = {mode: capabilities.referenceMode,
        channels: capabilities.referenceChannels};
      const surveillance = processingDevice.surveillance_channels || [];
      if (reference?.mode !== 'array_eigenbeam' ||
          !Array.isArray(reference.channels) ||
          reference.channels.length !== surveillance.length ||
          reference.channels.some((channel, index) => channel !== surveillance[index]))
        errors.push('coherent fusion requires array_eigenbeam reference channels to match surveillance channels');
      if (!Number.isFinite(processingDevice.array_geometry?.x_axis_bearing_deg_true))
        errors.push('coherent fusion requires x_axis_bearing_deg_true');
    }
  }
  if ((detection.integration_motion_bins ?? 0) > 0 &&
      (detection.integration_cpis ?? 1) !== 2)
    errors.push('integration_motion_bins requires integration_cpis: 2');
  if (trackBeforeDetect?.enable) {
    if (!detection.enable)
      errors.push('track-before-detect requires process.detection.enable');
    const fusion = processConfig.surveillance_fusion;
    if (residentMode &&
        ['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(fusion?.mode)) {
      const beams = fusion.beams ?? 12;
      const fine = processConfig.bearing?.enable ?
        (processConfig.bearing.scan_beams ?? 4 * beams) : 4 * beams;
      if (!Number.isInteger(beams) || beams <= 0 || !Number.isInteger(fine) ||
          fine < beams || fine > 127 || fine % beams !== 0)
        errors.push('Resident four/eight-CPI refinement requires a beam-count multiple through 127; default 4x refinement requires at most 31 surveillance beams, or enable bearing with a compatible scan_beams <= 127');
    }
  }
  if (multiIlluminator?.enabled !== true) {
    const budgetError = temporalBudgetError(ambiguityDopplerBins, delayBins, detection);
    if (budgetError) errors.push(`process.detection: ${budgetError}`);
  }
  if (tracker.beam_tolerance !== undefined) {
    if (!trackBeforeDetect?.enable && detection.integration_cpis === 2)
      errors.push('Two-CPI beam compatibility history is unsupported; select four/eight-CPI temporal integration or disable the explicit beam gate');
    const fusion = processConfig.surveillance_fusion;
    if (!tracker.enable || !fusion || fusion.mode === 'noncoherent')
      errors.push('tracker beam_tolerance requires coherent tracking');
    else if (Number.isFinite(fusion.beams) &&
        tracker.beam_tolerance > Math.floor(fusion.beams / 2))
      errors.push('tracker beam_tolerance cannot exceed half the beam count');
    if (!residentMode ||
        !((detection.integration_cpis ?? 1) > 1 || trackBeforeDetect?.enable))
      errors.push('Tracker beam_tolerance requires active resident coherent temporal tracking');
  }
  if (processConfig.bearing !== undefined) {
    const bearing = requireObject(processConfig.bearing, 'process.bearing');
    boolean(bearing.enable, 'process.bearing.enable');
    if (bearing.method !== undefined && !['coherent_beam_bank', 'covariance_mvdr'].includes(bearing.method))
      errors.push('process.bearing.method must be coherent_beam_bank or covariance_mvdr');
    if (bearing.reference_anchor !== undefined &&
        !['live_eigenbeam', 'fixed_site', 'fixed_site_projected'].includes(bearing.reference_anchor))
      errors.push('process.bearing.reference_anchor must be live_eigenbeam, fixed_site or fixed_site_projected');
    if (bearing.reference_anchor === 'fixed_site_projected') {
      const allFive = values => Array.isArray(values) && values.length === 5 &&
        values.every((channel, index) => channel === index);
      const reference = {mode: capabilities.referenceMode,
        channels: capabilities.referenceChannels};
      const fusion = processConfig.surveillance_fusion;
      if (processingDevice.channel_count !== 5 ||
          !allFive(processingDevice.surveillance_channels) || reference?.mode !== 'array_eigenbeam' ||
          !allFive(reference?.channels) || clutter.enable !== true ||
          !residentMode ||
          bearing.enable !== true || bearing.method !== 'covariance_mvdr' ||
          bearing.manifold_file !== undefined || fusion?.mode !== 'coherent_beam_bank' ||
          (fusion.direct_path_null_db ?? 0) !== 0)
        errors.push('fixed_site_projected requires enabled five-channel GPU-resident array-eigenbeam clutter cancellation, ideal covariance_mvdr bearing and coherent_beam_bank with direct_path_null_db: 0');
    }
    if (bearing.manifold_file !== undefined &&
        (typeof bearing.manifold_file !== 'string' || !bearing.manifold_file.trim()))
      errors.push('process.bearing.manifold_file must be a non-empty path');
    if (bearing.manifold_file !== undefined) {
      const heimdall=processingDevice.heimdall||{},geometry=processingDevice.array_geometry||{};
      const fixed=typeof heimdall.gain==='number'&&Number.isFinite(heimdall.gain)&&heimdall.gain>=0;
      const vector=Array.isArray(heimdall.channel_gains)&&
        heimdall.channel_gains.length===processingDevice.channel_count&&
        heimdall.channel_gains.every(value=>typeof value==='number'&&Number.isFinite(value)&&value>=0&&value<=50);
      if((heimdall.gain_control!==undefined&&heimdall.gain_control!=='fixed')||(!fixed&&!vector))
        errors.push('measured bearing manifold requires an explicit fixed requested gain vector');
      if(geometry.mapping_confirmed!==true||geometry.geometry_confirmed!==true||geometry.orientation_confirmed!==true)
        errors.push('measured bearing manifold requires confirmed channel mapping, geometry, and orientation');
    }
    if (bearing.channel_mapping !== undefined && (!Array.isArray(bearing.channel_mapping) ||
        bearing.channel_mapping.some(value => !Number.isInteger(value) || value < 0 || value > 7) ||
        new Set(bearing.channel_mapping).size !== bearing.channel_mapping.length))
      errors.push('process.bearing.channel_mapping must be a unique array of channel indices');
    if (bearing.manifold_recorder !== undefined) {
      const recorder = requireObject(bearing.manifold_recorder,
        'process.bearing.manifold_recorder');
      if (recorder.enabled !== undefined && typeof recorder.enabled !== 'boolean')
        oneOf(recorder.enabled, 'process.bearing.manifold_recorder.enabled');
      string(recorder.path, 'process.bearing.manifold_recorder.path');
      if (typeof recorder.path === 'string' && !path.isAbsolute(recorder.path))
        errors.push('process.bearing.manifold_recorder.path must be absolute');
      number(recorder.min_snr_db, 'process.bearing.manifold_recorder.min_snr_db');
      number(recorder.min_elevation_deg, 'process.bearing.manifold_recorder.min_elevation_deg');
      number(recorder.max_elevation_deg, 'process.bearing.manifold_recorder.max_elevation_deg');
      if (Number.isFinite(recorder.min_elevation_deg) && Number.isFinite(recorder.max_elevation_deg) &&
          recorder.max_elevation_deg < recorder.min_elevation_deg)
        errors.push('process.bearing.manifold_recorder.max_elevation_deg must not be below min_elevation_deg');
      number(recorder.max_delay_error_km, 'process.bearing.manifold_recorder.max_delay_error_km');
      number(recorder.max_doppler_error_hz, 'process.bearing.manifold_recorder.max_doppler_error_hz');
      number(recorder.max_truth_age_s, 'process.bearing.manifold_recorder.max_truth_age_s');
      number(recorder.max_file_mb, 'process.bearing.manifold_recorder.max_file_mb');
      number(recorder.max_files, 'process.bearing.manifold_recorder.max_files');
      if (recorder.enabled === true || recorder.enabled === 'on') {
        if (!capabilities.coherentSurveillance || !['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(
              processConfig.surveillance_fusion?.mode) || !detection.enable)
          errors.push('manifold recording requires multi-channel coherent detection');
        if (!capabilities.appliedGainMetadata)
          errors.push('manifold recording requires per-channel applied gain metadata');
        if (!root.truth?.adsb?.enabled)
          errors.push('manifold recording requires ADS-B truth');
        if (!Number.isInteger(ports.manifold) || ports.manifold < 1 || ports.manifold > 65535)
          errors.push('network.ports.manifold is required when manifold recording is enabled');
        const geometry = processingDevice.array_geometry;
        if (geometry?.mapping_confirmed !== true || geometry?.geometry_confirmed !== true ||
            geometry?.orientation_confirmed !== true || !Number.isFinite(geometry.x_axis_bearing_deg_true))
          errors.push('manifold recording requires confirmed array mapping, geometry, and orientation');
      }
    }
    if (bearing.enable) {
      if (!trackBeforeDetect?.enable || ![4, 8].includes(trackBeforeDetect.max_cpis ?? 8))
        errors.push('Bearing output requires enabled four/eight-CPI track-before-detect candidate maps');
      const fusion = processConfig.surveillance_fusion;
      if (!tracker.enable || !fusion || fusion.mode === 'noncoherent' ||
          !residentMode)
        errors.push('bearing output requires GPU-resident coherent tracking');
      if (!Number.isInteger(ports.bearing) || ports.bearing < 1 || ports.bearing > 65535)
        errors.push('network.ports.bearing is required when bearing output is enabled');
      const beams = processConfig.surveillance_fusion?.beams;
      if (bearing.scan_beams !== undefined && Number.isInteger(beams) &&
          (bearing.scan_beams < beams || bearing.scan_beams % beams !== 0))
        errors.push('process.bearing.scan_beams must be a multiple of the surveillance beam count');
      if (bearing.guard_radius !== undefined && bearing.training_radius !== undefined &&
          bearing.training_radius <= bearing.guard_radius)
        errors.push('process.bearing.training_radius must exceed guard_radius');
      if (bearing.channel_mapping !== undefined &&
          bearing.channel_mapping.length !== processingDevice.surveillance_channels?.length)
        errors.push('process.bearing.channel_mapping must include every surveillance channel once');
      const geometry = processingDevice.array_geometry;
      if (geometry?.mapping_confirmed !== true || geometry?.geometry_confirmed !== true ||
          geometry?.orientation_confirmed !== true)
        errors.push('bearing output requires confirmed mapping, geometry, and orientation');
    }
  }
  if (!(replay.state === true && Number.isInteger(replay.channel_count)) &&
      (device.type === 'Usrp' || device.type === 'RspDuo' ||
       device.type === 'HackRF')) {
    // Coherent paired receivers use the same processing-topology contract as
    // Kraken even though they do not expose an array channel-count control.
    // In dedicated mode this single channel is the native reference input and
    // the other coherent input is surveillance.
    const synthesis = processConfig.reference_synthesis === undefined ?
      {mode: 'dedicated', channels: [0]} :
      requireObject(processConfig.reference_synthesis, 'process.reference_synthesis');
    if (!['dedicated', 'array_eigenbeam'].includes(synthesis.mode))
      errors.push('process.reference_synthesis.mode must be dedicated or array_eigenbeam');
    const referenceChannels = synthesis.channels === undefined ? [] :
      integerArray(synthesis.channels, 'process.reference_synthesis.channels');
    const channelCount = replay.state === true && Number.isInteger(replay.channel_count) ?
      replay.channel_count : Array.isArray(device.gain) ? device.gain.length : 2;
    referenceChannels.forEach(channel => {
      if (Number.isInteger(channel) && channel >= channelCount)
        errors.push(`process.reference_synthesis.channels: input ${channel} is outside the configured channel count`);
    });
    if (synthesis.mode === 'dedicated' && referenceChannels.length !== 1)
      errors.push('dedicated reference synthesis requires exactly one reference channel');
    if (synthesis.mode === 'array_eigenbeam' && referenceChannels.length < 2)
      errors.push('array_eigenbeam reference synthesis requires at least two reference channels');
  }

  if (multiIlluminator?.doppler_window !== undefined &&
      multiIlluminator.doppler_window !== 'rectangular')
    errors.push('process.multi_illuminator.doppler_window supports only rectangular; other windows are not implemented');
  if (multiIlluminator?.enabled === true) {
    const fusion = processConfig.surveillance_fusion;
    const bearing = processConfig.bearing;
    const unsupported = [];
    if ((fusion?.mode ?? 'noncoherent') !== 'noncoherent') {
      for (const lane of multiIlluminator.illuminators ?? []) {
        if (lane?.enabled !== true) continue;
        if (!isObject(lane.transmitter_location))
          errors.push(`coherent illuminator ${lane.id} requires explicit transmitter_location`);
        if (lane.reference_channel !== undefined || lane.surveillance_channels !== undefined)
          errors.push(`coherent illuminator ${lane.id} requires matching array-reference/surveillance topology, not dedicated role overrides`);
      }
    }
    if ([true, 'on'].includes(bearing?.manifold_recorder?.enabled))
      unsupported.push('process.bearing.manifold_recorder.enabled');
    unsupported.forEach(field => errors.push(
      `${field} is not supported in multi-illuminator mode`));
  }
  if (multiIlluminator?.experimental_fm_reference_quality_filter !== undefined)
    boolean(multiIlluminator.experimental_fm_reference_quality_filter,
      'process.multi_illuminator.experimental_fm_reference_quality_filter');
  if (multiIlluminator?.experimental_fm_reference_quality_filter === true &&
      (multiIlluminator.enabled !== true ||
       !Array.isArray(multiIlluminator.illuminators) ||
       !multiIlluminator.illuminators.some(lane => lane?.enabled === true && lane.standard === 'FM')))
    errors.push('experimental FM reference quality filtering requires multi-illuminator processing with an enabled FM lane');

  string(network.ip, 'network.ip');
  if (typeof network.ip === 'string' && net.isIP(network.ip) !== 4)
    errors.push('network.ip must be an IPv4 address; the processor recording-control client does not support IPv6 URLs');
  for (const name of ['api','map','detection','track','timestamp','timing','iqdata'])
    number(ports[name], `network.ports.${name}`, {integer: true, min: 1, max: 65535});
  const portValues = [];
  Object.entries(ports).forEach(([name, value]) => {
    if (name === 'config') return; // Retired, never bound a listener.
    number(value, `network.ports.${name}`,
      {integer: true, min: 1, max: 65535});
    portValues.push(value);
  });
  if (new Set(portValues).size !== portValues.length)
    Object.entries(ports).forEach(([name, value]) => {
      if (portValues.filter(port => port === value).length > 1)
        errors.push(`network.ports.${name}: network ports must be unique`);
    });
  if (trackerForward) {
    boolean(trackerForward.enabled, 'network.tracker_forward.enabled');
    // A disabled live-site feed has no endpoint to validate or reconnect to.
    // Keep endpoint validation for every configuration that could open a socket.
    if (trackerForward.enabled === true) {
      string(trackerForward.host, 'network.tracker_forward.host');
      number(trackerForward.port, 'network.tracker_forward.port');
    }
    if (trackerForward.annotate_adsb !== undefined)
      boolean(trackerForward.annotate_adsb, 'network.tracker_forward.annotate_adsb');
  }
  if (unassistedTrackerForward) {
    boolean(unassistedTrackerForward.enabled,
      'network.tracker_forward.unassisted.enabled');
    if (unassistedTrackerForward.enabled === true) {
      string(unassistedTrackerForward.host,
        'network.tracker_forward.unassisted.host');
      number(unassistedTrackerForward.port, 'network.tracker_forward.unassisted.port');
    }
  }

  for (const [name, site] of [['rx', receiver], ['tx', transmitter]]) {
    number(site.latitude, `location.${name}.latitude`);
    number(site.longitude, `location.${name}.longitude`);
    number(site.altitude, `location.${name}.altitude`);
    string(site.name, `location.${name}.name`);
  }
  const truth = requireObject(root.truth, 'truth');
  if (receiver.latitude === transmitter.latitude && receiver.longitude === transmitter.longitude)
    advise('location', 'Set separate receiver and transmitter locations to use the map.');
  const adsb = requireObject(truth.adsb, 'truth.adsb');
  boolean(adsb.enabled, 'truth.adsb.enabled');
  string(adsb.tar1090, 'truth.adsb.tar1090');
  const validateAdsbSource = (source, key) => {
    const classification = classifyAdsbSource(source);
    if (classification.mode !== 'explicit') return source;
    try {
      if (typeof source === 'string' && /^local:(?!\d+(?:\/|$))/.test(source))
        throw new Error('Unknown local source');
      if (typeof source === 'string' && source.startsWith('exchange:')) {
        const endpoint = new URL(source.slice('exchange:'.length));
        if (!['http:', 'https:'].includes(endpoint.protocol) || !endpoint.hostname ||
            endpoint.username || endpoint.password || endpoint.search || endpoint.hash)
          throw new Error('Invalid exchange endpoint');
        return `exchange:${endpoint.href}`;
      }
      return sourceAddress(source).href;
    }
    catch (_) {
      errors.push(`${key}: select a local source, enter a readsb address, or use exchange:https://... without credentials or a query`);
      return null;
    }
  };
  validateAdsbSource(adsb.tar1090, 'truth.adsb.tar1090');
  if (adsb.sources !== undefined) {
    if (!Array.isArray(adsb.sources) || adsb.sources.length < 1 || adsb.sources.length > 8)
      errors.push('truth.adsb.sources must contain 1 to 8 source addresses');
    else {
      const unique = new Set();
      adsb.sources.forEach((source, index) => {
        string(source, `truth.adsb.sources.${index}`);
        const identity = validateAdsbSource(source, `truth.adsb.sources.${index}`);
        if (typeof source === 'string') {
          if (unique.has(identity)) errors.push('truth.adsb.sources must not contain duplicates');
          unique.add(identity);
        }
      });
      if (adsb.sources.length > 1 && adsb.sources.includes('auto'))
        errors.push('truth.adsb.sources: auto cannot be combined with explicit sources');
    }
  }
  if (adsb.source_timeout !== undefined) number(adsb.source_timeout, 'truth.adsb.source_timeout');
  if (adsb.aircraft_photo_provider !== undefined &&
      !['disabled', 'planespotters'].includes(adsb.aircraft_photo_provider))
    errors.push('truth.adsb.aircraft_photo_provider must be disabled or planespotters');
  if (adsb.poll_interval !== undefined) number(adsb.poll_interval, 'truth.adsb.poll_interval');
  if (adsb.smoothing_window !== undefined) number(adsb.smoothing_window, 'truth.adsb.smoothing_window');
  if (adsb.max_position_age !== undefined) number(adsb.max_position_age, 'truth.adsb.max_position_age');
  if (adsb.association_lag_cpis !== undefined) number(adsb.association_lag_cpis, 'truth.adsb.association_lag_cpis');
  if (adsb.uncertainty_association !== undefined) {
    const uncertainty = requireObject(adsb.uncertainty_association, 'truth.adsb.uncertainty_association');
    boolean(uncertainty.enable, 'truth.adsb.uncertainty_association.enable');
    try { require('./truth-uncertainty').policy(uncertainty); }
    catch (error) { errors.push(error.message); }
  }
  if (adsb.delay_tolerance !== undefined) number(adsb.delay_tolerance, 'truth.adsb.delay_tolerance');
  if (adsb.doppler_tolerance !== undefined) number(adsb.doppler_tolerance, 'truth.adsb.doppler_tolerance');
  if (truth.remote_id !== undefined) {
    const remoteId = requireObject(truth.remote_id, 'truth.remote_id');
    boolean(remoteId.enabled, 'truth.remote_id.enabled');
    string(remoteId.source, 'truth.remote_id.source');
    try {
      const endpoint = new URL(/^https?:\/\//i.test(remoteId.source) ? remoteId.source :
        `http://${remoteId.source}`);
      if (!['http:', 'https:'].includes(endpoint.protocol) || !endpoint.hostname ||
          endpoint.username || endpoint.password || endpoint.search || endpoint.hash)
        throw new Error('invalid endpoint');
    } catch (_) {
      errors.push('truth.remote_id.source must be an HTTP(S) Remote ID feed without credentials or a query');
    }
    if (remoteId.wifi_enabled !== undefined)
      boolean(remoteId.wifi_enabled, 'truth.remote_id.wifi_enabled');
  }
  ['map', 'detection'].forEach(key =>
    boolean(save[key], `save.${key}`));
  string(save.path, 'save.path');
  if (typeof save.path === 'string' && (!path.isAbsolute(save.path) || !save.path.endsWith('/')))
    errors.push('save.path must be a full directory path ending in /');
  if (pathPolicy?.kind === 'systemd-linux' && typeof save.path === 'string' &&
      path.isAbsolute(save.path) && !servicePathWithin(pathPolicy.recordingRoot, save.path))
    errors.push(`save.path must remain under ${pathPolicy.recordingRoot}/ for the packaged Linux service`);
  if (pathPolicy?.kind === 'systemd-linux' && typeof save.path === 'string' &&
      fs.existsSync(pathPolicy.recordingRoot)) {
    if (!fs.existsSync(save.path) || !fs.statSync(save.path).isDirectory())
      errors.push('save.path must be an existing recording folder created in VectorWarp');
    else {
      try { fs.accessSync(save.path, fs.constants.R_OK | fs.constants.W_OK); }
      catch (_) { errors.push('save.path is not readable and writable by the VectorWarp service'); }
    }
  }
  if (multiIlluminator?.enabled === true && Array.isArray(multiIlluminator.illuminators)) {
    for (const lane of multiIlluminator.illuminators.filter(item => item?.enabled === true)) {
      try {
        const rate = lane.output_sample_rate_hz;
        const effective = effectiveDelayConfig(root, rate, {scaleNeighborhoods: true}).process;
        const a = effective.ambiguity, d = effective.detection, c = effective.clutter;
        const samples = Math.round(cpi * rate), bins = a.delayMax - a.delayMin + 1;
        const dopplers = 1 + 2 * Math.floor((a.dopplerMax - a.dopplerMin) / 2 * samples / rate);
        const budgetError = temporalBudgetError(dopplers, bins, d);
        if (budgetError) errors.push(`process.multi_illuminator.${lane.id}: ${budgetError}`);
        const correlation = Math.floor(samples / dopplers);
        const cmin = c.followAmbiguity ? a.delayMin : c.delayMin;
        const cmax = c.followAmbiguity ? a.delayMax + 1 : c.delayMax;
        if (!Number.isSafeInteger(samples) || samples < 2000 || samples > INT32_MAX ||
            !Number.isInteger(dopplers) || dopplers < 1 || dopplers > 65535 ||
            bins < 2 || bins > 65534 || a.delayMin <= -correlation || a.delayMax >= correlation ||
            correlation + Math.max(Math.abs(a.delayMin), Math.abs(a.delayMax)) > 65535 ||
            d.minDelay < a.delayMin || d.minDelay > a.delayMax || 2 * d.nGuard + 2 >= bins ||
            (c.enable && (!(cmax > cmin) || cmax - cmin > samples || cmax - cmin + samples + 1 > INT32_MAX)))
          errors.push(`process.multi_illuminator.${lane.id}: effective delay/CPI geometry exceeds lane limits`);
      } catch (error) { errors.push(`process.multi_illuminator.${lane.id}: ${error.message}`); }
    }
  }
  return {valid: errors.length === 0, errors: [...new Set(errors)], notices, arrayGeometry,
    warnings: notices.map(notice => notice.message),
    estimatedMemoryBytes: Number.isFinite(estimatedBytes) ? estimatedBytes : null};
}

function writeConfigAtomically(filename, config) {
  const directory = path.dirname(filename);
  const temporary = path.join(directory,
    `.${path.basename(filename)}.${process.pid}.${Date.now()}.tmp`);
  const backup = `${filename}.bak`;
  const output = yaml.dump(config, {noRefs: true, lineWidth: 100,
    quotingType: '"', forceQuotes: false});
  try {
    const previous = fs.existsSync(filename) ? fs.statSync(filename) : null;
    fs.writeFileSync(temporary, output, {mode: previous ? previous.mode & 0o777 : 0o640, flag: 'wx'});
    if (previous) {
      try {
        fs.chownSync(temporary, previous.uid, previous.gid);
      } catch (error) {
        const currentUid = typeof process.getuid === 'function' ? process.getuid() : null;
        // A service in the config's shared group may replace a root-owned,
        // group-writable file, but cannot assume root ownership. Retain that
        // group while keeping the temporary file owned by the service account.
        if (error?.code !== 'EPERM' || currentUid === null || previous.uid === currentUid)
          throw error;
        fs.chownSync(temporary, currentUid, previous.gid);
      }
      fs.chmodSync(temporary, previous.mode & 0o777);
    }
    const descriptor = fs.openSync(temporary, 'r');
    fs.fsyncSync(descriptor);
    fs.closeSync(descriptor);
    if (fs.existsSync(filename)) fs.copyFileSync(filename, backup);
    fs.renameSync(temporary, filename);
    const directoryDescriptor = fs.openSync(directory, 'r');
    fs.fsyncSync(directoryDescriptor);
    fs.closeSync(directoryDescriptor);
  } finally {
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
}

module.exports = {KRAKEN_MAX_CHANNELS, getDeviceProfiles, validateConfig,
  linuxServicePathPolicy, FIELD_RULES, RETIRED_FIXED_SETTINGS, publicationPolicyError,
  writeConfigAtomically, effectiveRadarPlan, radarPlanErrors, assessResolvedRadarPlans};
