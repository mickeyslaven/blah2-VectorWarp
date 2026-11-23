'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const yaml = require('js-yaml');
const {getDeviceProfiles, validateConfig, writeConfigAtomically, FIELD_RULES,
  RETIRED_FIXED_SETTINGS, publicationPolicyError, effectiveRadarPlan, radarPlanErrors,
  assessResolvedRadarPlans} = require('./config-manager');
const {reconcile, recordErrors} = require('../html/js/kraken_geometry');
const {siteFingerprint} = require('./manifold-recorder');
const {legacyDelayReferenceRate, applyReceiverProfile,
  temporalConfirmationRequirements} = require('../html/js/config_profiles');
const {createReceiverJournal} = require('./receiver-journal');

// Source trees and macOS bundles use config/; native packages use
// config-examples/. Both contain the same immutable canonical first-run YAML.
// Never derive defaults from the operator-owned active configuration.
const canonicalPath = ['config', 'config-examples'].map(directory =>
  path.join(__dirname, '..', directory, 'config.yml')).find(filename => fs.existsSync(filename));
if (!canonicalPath) throw new Error('Canonical first-run configuration is missing from this installation.');
const canonicalDefaults = yaml.load(fs.readFileSync(canonicalPath, 'utf8'));

function setupDefaults() {
  const config = JSON.parse(JSON.stringify(canonicalDefaults));
  if (process.env.VECTORWARP_MACOS_STATE)
    config.process.bearing.manifold_recorder.path = path.join(
      process.env.VECTORWARP_MACOS_STATE, 'calibration', 'manifold-samples.jsonl');
  return config;
}

function configurationForReceiver(type, base) {
  const profile = getDeviceProfiles().find(item => item.type === type);
  if (!profile) throw new Error(`Unknown receiver profile: ${type}`);
  const config = JSON.parse(JSON.stringify(base === undefined ? setupDefaults() : base));
  return applyReceiverProfile(config, profile, base === undefined || config.capture?.device?.type === 'Unconfigured');
}

function stripLegacyNoOps(config) {
  if (!config || typeof config !== 'object') return config;
  for (const [name, accepted] of Object.entries(RETIRED_FIXED_SETTINGS)) {
    const parts = name.split('.'), leaf = parts.pop();
    const parent = parts.reduce((value, key) => value?.[key], config);
    if (parent && accepted(parent[leaf])) delete parent[leaf];
  }
  if (config.process?.tracker?.publication?.mode === 'native')
    delete config.process.tracker.publication;
  if (config.process?.data) delete config.process.data.overlap;
  if (config.process?.tracker) delete config.process.tracker.smooth;
  if (config.truth) delete config.truth.ais;
  // Retire observation-only lanes without promoting their unqualified output.
  // This runs in the existing atomic upgrade migration and save sanitizer.
  const detection = config.process?.detection;
  if (detection) {
    delete detection.temporal_shadow;
    delete detection.live_temporal_shadow;
    delete detection.doppler_ridge_scrubber;
  }
  if (config.process?.tracker) delete config.process.tracker.track_guided_shadow;
  if (config.process?.bearing) {
    delete config.process.bearing.frequency_resolved_shadow;
    delete config.process.bearing.manifold_automation;
  }
  if (config.process?.surveillance_fusion)
    delete config.process.surveillance_fusion.detection_manifold_automation;
  return config;
}

// A separately reviewed contract cannot be rewritten by Settings/presets or
// upgrade migrations. The explicit environment path also detects a missing pin.
function checkRadarPlan(config, filename, requiredFile = process.env.VECTORWARP_REQUIRED_RADAR_PLAN) {
  const contractFile = requiredFile || `${filename}.radar-plan.json`;
  try {
    const report = effectiveRadarPlan(config);
    if (!requiredFile && !fs.existsSync(contractFile))
      return {...report, qualification: 'unpinned', errors: []};
    if (fs.statSync(contractFile).size > 262144) throw new Error('Required radar plan exceeds 256 KiB');
    const requirement = JSON.parse(fs.readFileSync(contractFile, 'utf8'));
    const errors = radarPlanErrors(report, requirement);
    return {...report, qualification: errors.length ? 'rejected' : 'matched', contractFile,
      requirement_sha256: crypto.createHash('sha256').update(JSON.stringify(requirement)).digest('hex'),
      required_features: requirement.required_features, resolved_products: requirement.resolved_products, errors};
  } catch (error) {
    return {schema: 'vectorwarp-radar-plan-report-v1', qualification: 'rejected',
      contractFile, errors: [`Radar plan admission failed: ${error.message}`]};
  }
}

function readConfig(filename, {requiredPlanFile} = {}) {
  let raw = '';
  let exists = false;
  let parsed;
  let readError = null;
  try {
    if (fs.statSync(filename).size > 262144) throw new Error('Configuration exceeds 256 KiB.');
    raw = fs.readFileSync(filename, 'utf8');
    exists = true;
    parsed = yaml.load(raw, {schema: yaml.JSON_SCHEMA});
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
      throw new Error('The YAML root must contain configuration sections.');
    if (parsed.truth?.adsb && Object.prototype.hasOwnProperty.call(parsed.truth.adsb, 'adsb2dd'))
      delete parsed.truth.adsb.adsb2dd;
    stripLegacyNoOps(parsed);
    // Preserve bounded recorder collection defaults from released configs.
    const bearing = parsed.process?.bearing;
    if (bearing?.manifold_recorder && bearing.manifold_recorder.enabled === undefined)
      bearing.manifold_recorder.enabled = 'auto';
    // Detect cycles/aliases before recursive form rendering and validation.
    let nodes = 0;
    const visit = (value, parents = new Set(), depth = 0) => {
      if (++nodes > 4096) throw new Error('Too many configuration values.');
      if (!value || typeof value !== 'object') return;
      if (parents.has(value) || depth > 16) throw new Error('Recursive or deeply nested YAML is not supported.');
      const next = new Set(parents).add(value);
      Object.values(value).forEach(child => visit(child, next, depth + 1));
    };
    visit(parsed);
  } catch (error) {
    readError = error.code === 'ENOENT' ? 'No configuration file exists yet.' :
      `Configuration could not be read: ${error.message}`;
    parsed = null;
  }
  const defaults = setupDefaults();
  // A missing reference means legacy lane-local bins, never an inferred
  // 2-MS/s scale. Persist an unambiguous original rate in the migration.
  if (parsed?.process && parsed.process.delay_reference_sample_rate_hz === undefined)
    delete defaults.process.delay_reference_sample_rate_hz;
  // A released configuration without a profile is the legacy permissive
  // tracker. Do not silently migrate it merely by reading and saving another
  // setting; fresh configurations still start conservative.
  if (parsed?.process?.tracker && parsed.process.tracker.profile === undefined) {
    delete defaults.process.tracker.profile;
    delete defaults.process.tracker.initiate.min_snr_db;
  }
  const profile = getDeviceProfiles().find(item => item.type === parsed?.capture?.device?.type);
  if (profile) {
    defaults.capture.device = profile.device;
    defaults.capture.fs = profile.sampleRate;
    if (profile.process) {
      Object.assign(defaults.process, profile.process);
      defaults.process.performance.acceleration = 'auto';
      // Missing optional fields must use processor defaults, not the more
      // aggressive new-device preset (which opts into array synthesis).
      defaults.process.performance.surveillance_workers = 1;
      defaults.process.performance.fft_threads = 1;
    }
    if (profile.type === 'Kraken') {
      const count = Number.isInteger(parsed.capture.device.channel_count) ?
        Math.max(2, Math.min(8, parsed.capture.device.channel_count)) : 5;
      const reference = parsed.capture.device.reference_channel ?? 0;
      const mode = parsed.process?.reference_synthesis?.mode ?? 'dedicated';
      const channels = Array.from({length: count}, (_, i) => i);
      defaults.process.reference_synthesis.mode = 'dedicated';
      defaults.process.reference_synthesis.channels = mode === 'array_eigenbeam' ? channels : [reference];
      defaults.capture.device.surveillance_channels = channels.filter(channel =>
        mode === 'array_eigenbeam' || channel !== reference);
    }
  }
  const suppliedDefaults = [];
  const acceptsUnionType = (base, current, prefix) => {
    const type = FIELD_RULES[prefix]?.type;
    // The form has two deliberately polymorphic settings.  Their profile
    // defaults select only one member of the valid union, so comparing to the
    // default's JavaScript type would discard a valid persisted alternative.
    if (type === 'gain') return typeof current === 'string' || typeof current === 'number';
    if (type === 'serial') return typeof current === 'string' || Array.isArray(current);
    if (type === 'autoBoolean') return typeof current === 'boolean' ||
      ['auto', 'on', 'off'].includes(current);
    return false;
  };
  const optionalDefault = (value, prefix) => FIELD_RULES[prefix]?.optional ||
    (value && typeof value === 'object' && !Array.isArray(value) &&
      Object.entries(value).every(([key, child]) => optionalDefault(child, `${prefix}.${key}`)));
  const merge = (base, current, prefix = '') => {
    if (current === undefined) {
      if (!optionalDefault(base, prefix) &&
          prefix !== 'process.performance' && !prefix.startsWith('process.performance.') &&
          prefix !== 'process.multi_illuminator' && !prefix.startsWith('process.multi_illuminator.') &&
          prefix !== 'capture.aircraft_clips' && !prefix.startsWith('capture.aircraft_clips.') &&
          prefix !== 'truth.remote_id' && !prefix.startsWith('truth.remote_id.') &&
          !prefix.startsWith('process.reference_synthesis.target_protection_') &&
          prefix !== 'save.complex_cutout' && !prefix.startsWith('save.complex_cutout.') &&
          prefix !== 'network.ports.illuminator' &&
          prefix !== 'save.retention' && !prefix.startsWith('save.retention.'))
        suppliedDefaults.push(prefix);
      return base;
    }
    if (((Array.isArray(base) && !Array.isArray(current)) ||
        (base !== null && typeof base !== 'object' && typeof current !== typeof base)) &&
        !acceptsUnionType(base, current, prefix)) {
      suppliedDefaults.push(prefix);
      return base;
    }
    if (base && typeof base === 'object' && !Array.isArray(base) &&
        (!current || typeof current !== 'object' || Array.isArray(current))) {
      suppliedDefaults.push(prefix);
      return base;
    }
    if (!base || typeof base !== 'object' || Array.isArray(base) ||
        !current || typeof current !== 'object' || Array.isArray(current)) return current;
    const result = {...current};
    for (const [key, value] of Object.entries(base))
      result[key] = merge(value, current[key], prefix ? `${prefix}.${key}` : key);
    return result;
  };
  const config = stripLegacyNoOps(parsed ? merge(defaults, parsed) : defaults);
  // A multi-illuminator installation deliberately carries transmitter
  // geometry on each enabled lane.  The legacy global location.tx is only a
  // fallback for single-illuminator operation and must not force the first-run
  // assistant back open when every active lane has its own geometry.
  const laneTransmittersComplete = parsed?.process?.multi_illuminator?.enabled === true &&
    Array.isArray(parsed.process.multi_illuminator.illuminators) &&
    parsed.process.multi_illuminator.illuminators.some(lane => lane?.enabled === true) &&
    parsed.process.multi_illuminator.illuminators
      .filter(lane => lane?.enabled === true)
      .every(lane => lane.transmitter && typeof lane.transmitter.name === 'string' &&
        lane.transmitter.name.trim() && Number.isFinite(lane.transmitter.latitude) &&
        Number.isFinite(lane.transmitter.longitude) && Number.isFinite(lane.transmitter.altitude));
  const requiredDefaults = laneTransmittersComplete ?
    suppliedDefaults.filter(field => field !== 'location.tx') : suppliedDefaults;
  const validation = validateConfig(config);
  const radarPlan = checkRadarPlan(config, filename, requiredPlanFile);
  validation.errors.push(...radarPlan.errors);
  validation.valid = validation.errors.length === 0;
  return {config, revision: crypto.createHash('sha256').update(exists ? raw : 'missing').digest('hex'),
    exists, readError, suppliedDefaults: requiredDefaults, validation, radarPlan,
    setupRequired: Boolean(readError || requiredDefaults.length || !validation.valid)};
}

function writable(filename) {
  try {
    // Reject symlinks: atomic rename must not replace a symlink unexpectedly.
    if (fs.existsSync(filename)) {
      const file = fs.lstatSync(filename);
      // Root can pass access(W_OK) for chmod 0444. Respect an explicitly
      // read-only config even when the API process runs as root.
      if (!file.isFile() || !(file.mode & 0o222)) return false;
      fs.accessSync(filename, fs.constants.R_OK | fs.constants.W_OK);
    }
    if (!(fs.statSync(path.dirname(filename)).mode & 0o222)) return false;
    fs.accessSync(path.dirname(filename), fs.constants.W_OK);
    return true;
  } catch (_) { return false; }
}

// v0.1.28 exposed "localhost" as the USRP profile's USB selector. UHD device
// addresses are key=value maps, so an upgraded installation would otherwise
// retain a value that the stricter validator correctly rejects. Migrate only
// that exact released default, preserve all other operator settings, and keep
// the normal atomic backup/ownership behavior.
function migrateKnownDefaults(filename, {report = value => console.error(JSON.stringify(value))} = {}) {
  let file;
  try { file = fs.lstatSync(filename); }
  catch (error) {
    if (error.code === 'ENOENT') return false;
    throw error;
  }
  if (!file.isFile() || file.isSymbolicLink() || file.size > 262144) return false;
  let parsed, raw;
  try { raw = fs.readFileSync(filename, 'utf8'); parsed = yaml.load(raw, {schema: yaml.JSON_SCHEMA}); }
  catch (_) { return false; }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) return false;
  const revision = value => crypto.createHash('sha256').update(value).digest('hex');
  const previousRevision = revision(raw), receiverFingerprint = revision(JSON.stringify(parsed.capture || {}));
  const journal = createReceiverJournal(filename), predecessor = journal.load();
  const trackerConfirmationChanges = [];
  const beforeRetirement = JSON.stringify(parsed);
  stripLegacyNoOps(parsed);
  let changed = JSON.stringify(parsed) !== beforeRetirement;
  if (parsed.process && parsed.process.delay_reference_sample_rate_hz === undefined) {
    const reference = legacyDelayReferenceRate(parsed);
    if (reference !== null) {
      parsed.process.delay_reference_sample_rate_hz = reference;
      changed = true;
    }
  }
  if (parsed.capture?.device?.type === 'Usrp' && parsed.capture.device.address === 'localhost') {
    parsed.capture.device.address = 'type=b200';
    changed = true;
  }
  // Released deep-TBD configurations predate disjoint confirmation. Repair only
  // that already-enabled policy, never ordinary saves or a new preset selection.
  const required = temporalConfirmationRequirements(parsed);
  if (parsed.process?.detection?.enable === true &&
      parsed.process.detection.track_before_detect?.enable === true && required) {
    const {N, misses, minimumWindow, minimumMisses} = required;
    const nextN = Math.max(N, minimumWindow), nextMisses = Math.max(misses, minimumMisses);
    if (nextN !== N || nextMisses !== misses) {
      const candidate = readConfig(filename).config;
      candidate.process.tracker.initiate.N = nextN;
      candidate.process.tracker.delete = nextMisses;
      if (validateConfig(candidate).valid) {
        parsed.process.tracker.initiate.N = nextN;
        parsed.process.tracker.delete = nextMisses;
        trackerConfirmationChanges.push(
          {field: 'process.tracker.initiate.N', before: N, after: nextN},
          {field: 'process.tracker.delete', before: misses, after: nextMisses});
        changed = true;
      }
    }
  }
  if (!changed) return false;
  const planErrors = checkRadarPlan(parsed, filename).errors;
  if (planErrors.length) throw new Error(planErrors.join('; '));
  writeConfigAtomically(filename, parsed);
  const document = readConfig(filename);
  const receipt = {schema: 'vectorwarp-config-migration-v1', previousRevision,
    configRevision: document.revision, backup: `${filename}.bak`, trackerConfirmationChanges,
    receiverJournalPreserved: false};
  // Only an exact, previously verified Apply may cross this known migration.
  // Direct edits, changed receiver selectors and unresolved transactions retain
  // their interlock. Recheck the journal to avoid overwriting concurrent work.
  if (document.validation.valid && predecessor.configRevision === previousRevision &&
      predecessor.applicationVerified === true && predecessor.reconciliationRequired === false &&
      ['not-required', 'already-matched', 'synchronized'].includes(predecessor.state) &&
      receiverFingerprint === revision(JSON.stringify(parsed.capture || {})) &&
      JSON.stringify(journal.load()) === JSON.stringify(predecessor)) {
    receipt.receiverJournalPreserved = true;
    journal.write({...predecessor, configRevision: document.revision,
      receipt: {...predecessor.receipt, configRevision: document.revision,
        configMigration: receipt}});
  }
  report(receipt);
  return true;
}

// Package activation can restore the API more than once when systemd retries
// an early failure.  A root-created, short expiry preserves the conffile for
// those starts without turning a killed package transaction into a permanent
// migration bypass.
function preserveConfigBytesDuringUpgrade(environment = process.env,
  uptimeSeconds) {
  const now = Math.floor(Number(uptimeSeconds));
  const until = Number(environment.VECTORWARP_PRESERVE_CONFIG_BYTES_UNTIL_UPTIME);
  return Number.isSafeInteger(now) && Number.isSafeInteger(until) &&
    until >= now && until <= now + 300;
}

function saveConfig(filename, candidate, expectedRevision) {
  const before = readConfig(filename);
  if (expectedRevision !== before.revision) {
    const error = new Error('The config file changed. Reload settings before saving; your edits have not been written.');
    error.status = 409;
    throw error;
  }
  const metadataErrors = recordErrors(candidate?.capture?.device?.array_geometry);
  const publicationError = publicationPolicyError(candidate);
  if (publicationError) metadataErrors.push(publicationError);
  for (const [name, accepted] of Object.entries(RETIRED_FIXED_SETTINGS)) {
    const value = name.split('.').reduce((item, key) => item?.[key], candidate);
    if (value !== undefined && !accepted(value))
      metadataErrors.push(`${name} is retired and its requested alternative is unsupported`);
  }
  if (metadataErrors.length) {
    const error = new Error(metadataErrors.join(' '));
    error.status = 422;
    throw error;
  }
  const prepared = stripLegacyNoOps(reconcile(JSON.parse(JSON.stringify(candidate)), before.config));
  const previousGain = before.config.capture?.device?.heimdall?.gain ?? 'keep';
  const selectedGain = prepared.capture?.device?.heimdall?.gain ?? 'keep';
  const bearing = prepared.process?.bearing;
  if (selectedGain !== previousGain && bearing &&
      (bearing.manifold_file || before.config.process?.bearing?.manifold_file)) {
    if (bearing.manifold_file === before.config.process?.bearing?.manifold_file) {
      const error = new Error('The selected bearing manifold must be requalified for the new gain. Explicitly select a matching calibration before saving.');
      error.status = 422;
      throw error;
    }
  }
  const automaticPath = before.config.process?.bearing?.manifold_recorder?.path;
  if (automaticPath && prepared.process?.bearing?.manifold_file === `${automaticPath}.qualified.json` &&
      siteFingerprint(before.config) !== siteFingerprint(prepared)) {
    const error = new Error('The selected bearing manifold does not match the changed installation. Explicitly select a qualified calibration before saving.');
    error.status = 422;
    throw error;
  }
  const planErrors = checkRadarPlan(prepared, filename).errors;
  if (planErrors.length) {
    const error = new Error(planErrors.join('; '));
    error.status = 422;
    throw error;
  }
  writeConfigAtomically(filename, prepared);
  return readConfig(filename);
}

module.exports = {readConfig, writable, saveConfig, setupDefaults, configurationForReceiver,
  migrateKnownDefaults, preserveConfigBytesDuringUpgrade, stripLegacyNoOps, checkRadarPlan};

// Read-only deployment/CI interface. pin-preview emits an artifact for review;
// it never writes or weakens the independently managed requirement file.
if (require.main === module) {
  try {
    const [command, filename, ...args] = process.argv.slice(2);
    if (!['plan', 'pin-preview', 'accept', 'accept-runtime'].includes(command) || !filename ||
        (command === 'accept' && args.length !== 1) || (command === 'plan' && args.length) ||
        (command === 'accept-runtime' && args.length !== 2) ||
        (command === 'pin-preview' && !args.length))
      throw new Error('usage: config-store.js plan CONFIG | pin-preview CONFIG feature=JSON [...] | accept CONFIG REQUIREMENT | accept-runtime CONFIG REQUIREMENT REPORT');
    const document = readConfig(filename, {requiredPlanFile: command.startsWith('accept') ? args[0] : undefined});
    if (document.readError || !document.validation.valid)
      throw new Error(document.readError || document.validation.errors.join('; '));
    const required_features = {};
    if (command === 'pin-preview') {
      for (const argument of args) {
        const equals = argument.indexOf('=');
        if (equals < 1) throw new Error('Feature assertions must be feature=JSON');
        required_features[argument.slice(0, equals)] = JSON.parse(argument.slice(equals + 1));
      }
      const requirement = {schema: 'vectorwarp-radar-plan-requirements-v1',
        semantic_sha256: document.radarPlan.semantic_sha256, required_features};
      const errors = radarPlanErrors(document.radarPlan, requirement);
      if (errors.length) throw new Error(errors.join('; '));
      console.log(JSON.stringify(requirement, null, 2));
    } else if (command === 'accept-runtime') {
      if (fs.statSync(args[1]).size > 262144) throw new Error('Runtime radar plan report exceeds 256 KiB');
      const runtime = JSON.parse(fs.readFileSync(args[1], 'utf8'));
      if (runtime.loadedRevision !== document.revision ||
          runtime.loaded?.semantic_sha256 !== document.radarPlan.semantic_sha256)
        throw new Error('Runtime radar plan does not attest this exact loaded configuration');
      const result = assessResolvedRadarPlans(document.radarPlan, runtime.observation);
      if (result.errors.length) throw new Error(result.errors.join('; '));
      console.log(JSON.stringify(result, null, 2));
    } else console.log(JSON.stringify(document.radarPlan, null, 2));
  } catch (error) { console.error(error.message); process.exitCode = 78; }
}
