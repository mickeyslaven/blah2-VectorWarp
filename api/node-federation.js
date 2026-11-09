'use strict';

const fs = require('fs');
const path = require('path');
const net = require('net');
const os = require('os');
const http = require('http');
const https = require('https');
const {geometryCatalog, CooperativeDetectionTracker} = require('./cooperative-detection-tracker');

const HTTP_AGENT = new http.Agent({keepAlive: true, maxSockets: 16, maxFreeSockets: 8,
  timeout: 5000});
const HTTPS_AGENT = new https.Agent({keepAlive: true, maxSockets: 16, maxFreeSockets: 8,
  timeout: 5000});

// This is deliberately an exact path list: adding a GET endpoint requires review.
const READS = new Set(['/api/federation/capabilities', '/api/system/status', '/api/upstream/status', '/api/illuminators/live',
  '/api/map', '/api/radar/frame', '/api/detection', '/api/tracker', '/api/tracker/fused', '/api/tracker/native',
  '/api/statistics', '/api/live-scorecard', '/api/live-scorecard/provisional',
  '/api/live-scorecard/association-basis', '/api/association/passes',
  '/api/recordings', '/api/bearing', '/api/bearing/diagnostic', '/api/array-calibration/status',
  '/api/locations', '/api/timestamp', '/api/timing',
  '/api/presentation/status', '/api/runtime/config', '/api/adsb', '/api/adsb/status',
  '/api/adsb/delay-doppler', '/api/remote-id', '/api/remote-id/status',
  '/api/remote-id/delay-doppler', '/api/radar/coverage', '/api/federation/cooperative',
  '/api/federation/statistics-summary', '/api/live-aircraft-associations',
  '/stash/map', '/stash/detection',
  '/stash/iqdata', '/stash/timing']);
const MAX_BYTES = 16 * 1024 * 1024;
const MAX_ACTIVE = 8;

function nodeCapabilities(config = {}) {
  const multi = config.process?.multi_illuminator;
  const standards = [...new Set((multi?.illuminators || [])
    .filter(item => item?.enabled === true && typeof item.standard === 'string')
    .map(item => item.standard))].sort();
  const resources = ['status', 'runtime-config-summary', 'map', 'detections',
    'tracks', 'cooperative-tracks', 'statistics', 'timing', 'recordings'];
  if (multi?.enabled === true) resources.push('illuminators', 'fused-tracks');
  if (config.process?.bearing?.enable === true) resources.push('bearing');
  if (config.truth?.adsb?.enabled === true) resources.push('adsb');
  if (config.truth?.remote_id?.enabled === true) resources.push('remote-id');
  return {schema: 'vectorwarp-node-capabilities-v1', protocol_version: 1,
    federation: {read_only: true, aggregate_identity_merge: false,
      mutations_local_only: true},
    resources, receiver: {type: config.capture?.device?.type || null,
      channel_count: config.capture?.device?.channel_count ?? null},
    processing: {multi_illuminator: multi?.enabled === true,
      illuminator_standards: standards,
      atsc_reference_modes: ['automatic', 'raw', 'pilot_aided'],
      atsc3_reference_modes: ['automatic', 'raw'],
      atsc_decision_directed_available: false,
      reference_reconstruction: {
        raw: {status: 'stable', standards: ['*']},
        fm_modulation_aided: {status: 'experimental', live_selectable: false},
        atsc_8vsb: {status: 'pilot-projection-qualified', live_selectable: true,
          completed_stages: ['heldout-pilot-projection', 'raw-clutter-separation',
            'field-sync', 'confidence-gated-symbol-reconstruction'],
          full_reconstruction_live_selectable: false},
        atsc3_ofdm: {status: 'experimental-rejected', live_selectable: false,
          completed_stages: ['standards-rate-resampling', 'fft-guard-acquisition',
            'confidence-gated-cyclic-prefix-reconstruction'],
          required_next_stage: 'full-l1-pilot-decision-directed-remodulation'},
        dab_ofdm: {status: 'experimental-planned', live_selectable: false},
        generic_digital: {status: 'experimental-planned', live_selectable: false}
      }},
    sample_rate_hz: config.capture?.fs ?? null,
    center_frequency_hz: config.capture?.fc ?? null};
}

function registry(filename) {
  if (!filename || !fs.existsSync(filename)) return [];
  const resolved = fs.realpathSync(filename);
  const repo = path.resolve(__dirname, '..');
  if (resolved === repo || resolved.startsWith(repo + path.sep)) throw new Error('Registry must be outside the application tree');
  const stat = fs.statSync(resolved);
  if (!stat.isFile() || (stat.mode & 0o077)) throw new Error('Registry must be a private file');
  const data = JSON.parse(fs.readFileSync(resolved, 'utf8'));
  if (!Array.isArray(data.nodes) || data.nodes.length > 16) throw new Error('Invalid node registry');
  const ids = new Set(['local', 'aggregate']);
  return data.nodes.map(node => {
    if (!node || typeof node.id !== 'string' || !/^[a-z0-9][a-z0-9_-]{0,63}$/.test(node.id) || ids.has(node.id) ||
        typeof node.label !== 'string' || !node.label.trim() || node.label.length > 80)
      throw new Error('Invalid node identity');
    ids.add(node.id);
    if (typeof node.expectedIdentity !== 'string' || !/^[a-z0-9][a-z0-9_-]{0,63}$/.test(node.expectedIdentity)) throw new Error('Expected node identity is required');
    const url = new URL(node.url);
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password ||
        url.pathname !== '/' || url.search || url.hash) throw new Error('Invalid node URL');
    let token = null;
    if (node.tokenFile) {
      if (!path.isAbsolute(node.tokenFile)) throw new Error('Token path must be absolute');
      const tokenPath = fs.realpathSync(node.tokenFile);
      if (tokenPath === repo || tokenPath.startsWith(repo + path.sep)) throw new Error('Token must be outside the application tree');
      const tokenStat = fs.statSync(tokenPath);
      if (!tokenStat.isFile() || (tokenStat.mode & 0o077) || tokenStat.size > 4096) throw new Error('Token file must be private');
      token = fs.readFileSync(tokenPath, 'utf8').trim();
      if (!/^[\x21-\x7e]+$/.test(token)) throw new Error('Invalid token');
    }
    return {id: node.id, label: node.label, origin: url.origin, token,
      expectedIdentity: node.expectedIdentity, stored: {...node}};
  });
}

function localNodeOrigin(host, port) {
  const address = String(host || '').trim();
  const family = net.isIP(address);
  const numericPort = Number(port);
  if (!family || !Number.isInteger(numericPort) || numericPort < 1 || numericPort > 65535)
    throw new Error('Enter a local IP address and port');
  const local = family === 4 ? (() => {
    const octets = address.split('.').map(Number);
    return octets[0] === 10 || octets[0] === 127 || octets[0] === 192 && octets[1] === 168 ||
      octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31 ||
      octets[0] === 169 && octets[1] === 254 ||
      octets[0] === 100 && octets[1] >= 64 && octets[1] <= 127;
  })() : /^(?:::1|f[cd][0-9a-f]{2}:|fe[89ab][0-9a-f]:)/i.test(address);
  if (!local) throw new Error('Only private, loopback, link-local, or tailnet IP addresses are allowed');
  return `http://${family === 6 ? `[${address}]` : address}:${numericPort}`;
}

function probeJson(origin, pathname, timeoutMs = 2500) {
  return new Promise((resolve, reject) => {
    let settled = false, size = 0, deadline; const chunks = [];
    const finish = (error, value) => { if (settled) return; settled = true;
      clearTimeout(deadline);
      error ? reject(error) : resolve(value); };
    const target = new URL(pathname, origin);
    const request = (target.protocol === 'https:' ? https : http).get(target, {timeout: timeoutMs,
      agent: target.protocol === 'https:' ? HTTPS_AGENT : HTTP_AGENT,
      headers: {Accept: 'application/json', 'Accept-Encoding': 'identity'}}, response => {
      if (response.statusCode !== 200) { response.resume();
        return finish(new Error(`Node probe returned HTTP ${response.statusCode}`)); }
      response.on('data', chunk => { size += chunk.length;
        // A compact multi-channel association basis can exceed 1 MiB before
        // transport compression because it retains per-pass direction cells.
        // Keep the read bounded while allowing a full local-node score basis.
        if (size > 2 * 1024 * 1024) request.destroy(new Error('Node probe response is too large'));
        else chunks.push(chunk); });
      response.on('end', () => { try { finish(null, {value: JSON.parse(Buffer.concat(chunks)),
        identity: response.headers['x-vectorwarp-node-id'] || null}); }
      catch (_) { finish(new Error('Node probe returned invalid JSON')); } });
      response.on('error', finish);
    });
    request.on('timeout', () => request.destroy(new Error('Node probe timed out')));
    request.on('error', finish);
    deadline = setTimeout(() => request.destroy(new Error('Node probe deadline exceeded')),
      timeoutMs);
  });
}

async function discoverLocalNode(host, port, label) {
  const origin = localNodeOrigin(host, port);
  const [identityResult, capabilitiesResult, statusResult] = await Promise.all([
    probeJson(origin, '/api/federation/identity'),
    probeJson(origin, '/api/federation/capabilities'),
    probeJson(origin, '/api/system/status?summary=1')]);
  const identity = identityResult.value?.identity;
  if (!/^[a-z0-9][a-z0-9_-]{0,63}$/.test(identity || '') ||
      identity === 'local' || identity === 'aggregate' ||
      identityResult.identity !== identity || capabilitiesResult.identity !== identity ||
      statusResult.identity !== identity)
    throw new Error('The node did not provide one consistent VectorWarp identity');
  if (capabilitiesResult.value?.schema !== 'vectorwarp-node-capabilities-v1' ||
      typeof statusResult.value?.radar !== 'string')
    throw new Error('The address is not a compatible VectorWarp node');
  const cleanLabel = typeof label === 'string' && label.trim() ? label.trim() : identity;
  if (cleanLabel.length > 80) throw new Error('Node label is too long');
  return {id: identity, label: cleanLabel, expectedIdentity: identity, url: origin};
}

function saveRegistry(filename, nodes) {
  const directory = path.dirname(filename);
  fs.mkdirSync(directory, {recursive: true, mode: 0o750});
  const temporary = `${filename}.new`;
  fs.writeFileSync(temporary, JSON.stringify({nodes: nodes.map(node => node.stored || {
    id: node.id, label: node.label, expectedIdentity: node.expectedIdentity, url: node.origin
  })}, null, 2) + '\n', {mode: 0o600});
  fs.chmodSync(temporary, 0o600);
  fs.renameSync(temporary, filename);
}

function defaultRegistryPath(environment = process.env, platform = process.platform,
    home = os.homedir(), uid = typeof process.getuid === 'function' ? process.getuid() : null) {
  if (environment.VECTORWARP_NODE_REGISTRY) return environment.VECTORWARP_NODE_REGISTRY;
  if (platform === 'darwin')
    return path.join(home, 'Library', 'Application Support', 'VectorWarp', 'nodes.json');
  if (platform === 'win32')
    return path.join(environment.APPDATA || path.join(home, 'AppData', 'Roaming'),
      'VectorWarp', 'nodes.json');
  if (uid === 0) return '/etc/vectorwarp/nodes.json';
  return path.join(environment.XDG_CONFIG_HOME || path.join(home, '.config'),
    'vectorwarp', 'nodes.json');
}

function configuredApiPort(config = {}, environment = process.env) {
  const setupPort = Number(environment.BLAH2_SETUP_PORT || 3000);
  const configured = Number(config?.network?.ports?.api);
  return environment.BLAH2_SETUP_PORT ? setupPort :
    Number.isInteger(configured) && configured > 0 && configured <= 65535 ? configured : setupPort;
}

function safeConfig(config) {
  const p = config.process || {}, c = config.capture || {}, t = config.truth || {};
  return {capture: {fs: c.fs, fc: c.fc, device: {type: c.device?.type,
    channel_count: c.device?.channel_count, reference_channel: c.device?.reference_channel,
    surveillance_channels: c.device?.surveillance_channels}},
    location: config.location, process: {data: p.data,
      delay_reference_sample_rate_hz: p.delay_reference_sample_rate_hz,
      ambiguity: p.ambiguity, clutter: p.clutter, detection: p.detection,
      tracker: p.tracker, bearing: p.bearing, surveillance_fusion: p.surveillance_fusion,
      multi_illuminator: p.multi_illuminator},
    truth: {adsb: {enabled: t.adsb?.enabled,
      max_position_age: t.adsb?.max_position_age,
      association_lag_cpis: t.adsb?.association_lag_cpis,
      delay_tolerance: t.adsb?.delay_tolerance,
      doppler_tolerance: t.adsb?.doppler_tolerance,
      provisional_event_time_uncertainty_s: t.adsb?.provisional_event_time_uncertainty_s},
      remote_id: {enabled: t.remote_id?.enabled}}};
}

function cooperativeSources(node, config) {
  const capture = config?.capture || {}, process = config?.process || {};
  const multi = process.multi_illuminator;
  if (multi?.enabled === true) return (multi.illuminators || [])
    .filter(item => item?.enabled === true).map(item => ({
      key: `${node.id}:${item.id}`, node: node.id, channel: item.id,
      receiver: config?.location?.rx, transmitter: item.transmitter,
      receiver_site_id: config?.location?.rx?.site_id,
      transmitter_site_id: item.transmitter?.site_id ?? item.transmitter_site_id,
      frequency_hz: Number(item.frequency_hz),
      sample_rate_hz: Number(item.output_sample_rate_hz),
      detection_path: `/api/detection?illuminator=${encodeURIComponent(item.id)}`}));
  return [{key: `${node.id}:primary`, node: node.id, channel: 'primary',
    receiver: config?.location?.rx, transmitter: config?.location?.tx,
    receiver_site_id: config?.location?.rx?.site_id,
    transmitter_site_id: config?.location?.tx?.site_id,
    frequency_hz: Number(capture.fc),
    sample_rate_hz: Number(capture.fs),
    detection_path: '/api/detection'}];
}

function compactPass(value) {
  if (!value || typeof value !== 'object') return null;
  const detected = value.detected === true ||
    ['detected_not_tracked', 'tracked'].includes(value.outcome);
  const tracked = value.tracked === true || value.outcome === 'tracked';
  return {pass_id: value.pass_id ?? null, hex: value.hex ?? null,
    flight: value.flight ?? null, started_at: value.started_at ?? null,
    finalized_at: value.finalized_at ?? null, outcome: value.outcome ?? null,
    channel: value.channel ?? null,
    eligible_cpis: Number(value.eligible_cpis) || (value.eligible === false ? 0 : 3),
    detected_cpis: Number(value.detected_cpis) || (detected ? 3 : 0),
    tracked_cpis: Number(value.tracked_cpis) || (tracked ? 3 : 0),
    eligible: value.eligible !== false, detected, tracked};
}

function compactNodeStatistics(entry, status, timing, scorecard, associationPasses = null) {
  const aggregate = scorecard?.overall_association_statistics;
  const unique = aggregate?.unique_aircraft_passes;
  const lifetime = scorecard?.current_setup_lifetime || {};
  const association = unique ? {eligible: Number(unique.eligible_adsb_passes) || 0,
    detected: Number(unique.detection?.associated_passes) || 0,
    tracked: Number(unique.tracking?.associated_passes) || 0} : aggregate ?
    {eligible: Number(aggregate.eligible_adsb_passes) || 0,
      detected: Number(aggregate.detection?.associated_passes) || 0,
      tracked: Number(aggregate.tracking?.associated_passes) || 0} :
    {eligible: Number(lifetime.eligible_passes) || 0,
      detected: Number(lifetime.detected_passes) || 0,
      tracked: Number(lifetime.tracked_passes) || 0};
  const channelRows = Array.isArray(scorecard?.channels) ? scorecard.channels :
    (Array.isArray(scorecard?.illuminators) ? scorecard.illuminators : []);
  const channels = channelRows.length ? channelRows.map((row, index) => {
    const value = row?.association_statistics || {};
    return {id: row?.identity?.illuminator || row?.selected_channel || `channel-${index + 1}`,
      eligible: Number(value.eligible_adsb_passes) || 0,
      detected: Number(value.detection?.associated_passes) || 0,
      tracked: Number(value.tracking?.associated_passes) || 0};
  }) : [{id: 'primary', ...association}];
  const scorecardPasses = channelRows.length ? channelRows.flatMap(row =>
    (row?.passes || row?.recent_scored_passes || row?.recent_finalized_passes || [])
      .map(pass => ({...pass, channel: pass.channel || row?.identity?.illuminator ||
        row?.selected_channel || 'primary'}))) :
    (scorecard?.recent_scored_passes || scorecard?.recent_finalized_passes || []);
  const passSources = Array.isArray(associationPasses?.passes) ?
    associationPasses.passes : scorecardPasses;
  const passes = passSources.map(compactPass).filter(Boolean);
  const tracking = status?.featureDiagnostics?.trackingPerformance || status?.trackingPerformance || {};
  const cadenceMs = Number(status?.featureDiagnostics?.processing?.configuredCpiMs) || null;
  const rawBacklogMs = Number(timing?.capture_backlog_ms);
  const readyCpis = Number(timing?.capture_queue_ready_cpis);
  // captureBacklogSamples includes the CPI currently being filled on receivers
  // that cannot expose queue occupancy separately (notably Heimdall). That is
  // required acquisition staging, not work waiting for the processor. Prefer
  // the explicit ready-CPI count; otherwise remove one in-flight CPI so all
  // receiver types report the same queued-work definition.
  const queuedBacklogMs = Number.isFinite(readyCpis) && readyCpis >= 0 && cadenceMs > 0 ?
    readyCpis * cadenceMs : Number.isFinite(rawBacklogMs) && cadenceMs > 0 ?
      Math.max(0, rawBacklogMs - cadenceMs) : Number.isFinite(rawBacklogMs) ? rawBacklogMs : null;
  return {id: entry.id, label: entry.label || entry.id, local: entry.local === true,
    available: true, radar: status?.radar || 'unknown', processor_fresh: status?.processorFresh === true,
    config_match: status?.configRevision && status?.loadedRevision ?
      status.configRevision === status.loadedRevision : true,
    tracking_status: tracking.status || 'unknown', channels: channels.map(row => ({...row,
      node: entry.id, key: `${entry.id}:${row.id}`})), association, passes,
    timing: {cpi_ms: Number(timing?.cpi) || null,
      cadence_ms: cadenceMs,
      backlog_ms: queuedBacklogMs,
      raw_capture_backlog_ms: Number.isFinite(rawBacklogMs) ? rawBacklogMs : null,
      drops: Array.isArray(timing?.captureDroppedSamples) ? timing.captureDroppedSamples : [],
      retired_cpis: Number(timing?.capture_retired_cpis_total) || 0,
      retired_samples: Number(timing?.capture_retired_samples_total) || 0,
      acceleration: timing?.acceleration?.active || null}};
}

function deduplicatePasses(nodes) {
  const groups = [];
  const rows = nodes.flatMap(node => (node.passes || []).map(pass => ({node, pass})))
    .filter(({pass}) => String(pass.hex || pass.flight || '').trim() &&
      Number.isFinite(Number(pass.started_at)))
    .sort((left, right) => {
      const identity = String(left.pass.hex || left.pass.flight).trim().toLowerCase()
        .localeCompare(String(right.pass.hex || right.pass.flight).trim().toLowerCase());
      return identity || Number(left.pass.started_at) - Number(right.pass.started_at);
    });
  for (const {node, pass} of rows) {
    const identity = String(pass.hex || pass.flight).trim().toLowerCase();
    const startedAt = Number(pass.started_at);
    const finalizedAt = Number(pass.finalized_at) || startedAt;
    let group = groups.at(-1);
    if (!group || group.identity !== identity || startedAt > group.finalized_at + 30000)
      group = null;
    if (!group) { group = {identity, started_at: startedAt, finalized_at: finalizedAt,
      nodes: [], passes: []}; groups.push(group); }
    group.started_at = Math.min(group.started_at, startedAt);
    group.finalized_at = Math.max(group.finalized_at, finalizedAt);
    group.nodes.push(node.id); group.passes.push(pass);
  }
  return groups.map(group => ({identity: group.identity,
    started_at: Math.min(...group.passes.map(pass => Number(pass.started_at))),
    finalized_at: Math.max(...group.passes.map(pass => Number(pass.finalized_at) || 0)),
    nodes: [...new Set(group.nodes)].sort(),
    channels: [...new Set(group.passes.map(pass => pass.channel).filter(Boolean))].sort(),
    eligible: group.passes.some(pass => pass.eligible === true || pass.eligible_cpis >= 3),
    detected: group.passes.some(pass => pass.detected === true ||
      ['detected_not_tracked', 'tracked'].includes(pass.outcome)),
    tracked: group.passes.some(pass => pass.tracked === true || pass.outcome === 'tracked')}));
}

function alignNodeStatistics(nodes) {
  const starts = nodes.map(node => Math.min(...(node.passes || [])
    .map(pass => Number(pass.started_at)).filter(Number.isFinite)))
    .filter(Number.isFinite);
  const commonStart = starts.length === nodes.length ? Math.max(...starts) : null;
  return {commonStart, nodes: nodes.map(node => {
    const passes = (node.passes || []).filter(pass => commonStart === null ||
      (Number(pass.finalized_at) || Number(pass.started_at) || 0) >= commonStart);
    const channelIds = new Set((node.channels || []).map(channel => channel.id));
    for (const pass of passes) channelIds.add(pass.channel || 'primary');
    const channels = [...channelIds].map(id => {
      const values = passes.filter(pass => (pass.channel || 'primary') === id);
      const eligible = values.filter(pass => pass.eligible === true || pass.eligible_cpis >= 3).length;
      const detected = values.filter(pass => pass.detected === true ||
        ['detected_not_tracked', 'tracked'].includes(pass.outcome)).length;
      const tracked = values.filter(pass => pass.tracked === true || pass.outcome === 'tracked').length;
      return {id, eligible, detected, tracked, node: node.id, key: `${node.id}:${id}`};
    });
    const unique = deduplicatePasses([{...node, passes}]).filter(pass => pass.eligible);
    const detected = unique.filter(pass => pass.detected);
    const tracked = detected.filter(pass => pass.tracked);
    return {...node, passes, channels, association: {eligible: unique.length,
      detected: detected.length, tracked: tracked.length}};
  })};
}

function combineNodeStatistics(nodes, cooperative = null) {
  const rawAvailable = nodes.filter(node => node?.available);
  const alignment = alignNodeStatistics(rawAvailable);
  const available = alignment.nodes;
  const channels = available.flatMap(node => node.channels || []);
  const association = channels.reduce((sum, row) => ({eligible: sum.eligible + row.eligible,
    detected: sum.detected + row.detected, tracked: sum.tracked + row.tracked}),
  {eligible: 0, detected: 0, tracked: 0});
  const passGroups = [];
  // Kept structurally for compatibility with older coverage; the canonical
  // deduplicator below owns the aligned pass population.
  const passRows = [].flatMap(node => (node.passes || []).map(pass => ({node, pass})))
    .filter(({pass}) => String(pass.hex || pass.flight || '').trim() &&
      Number.isFinite(Number(pass.started_at)))
    .sort((left, right) => {
      const identity = String(left.pass.hex || left.pass.flight).trim().toLowerCase()
        .localeCompare(String(right.pass.hex || right.pass.flight).trim().toLowerCase());
      return identity || Number(left.pass.started_at) - Number(right.pass.started_at);
    });
  for (const {node, pass} of passRows) {
    const identity = String(pass.hex || pass.flight || '').trim().toLowerCase();
    const startedAt = Number(pass.started_at);
    const finalizedAt = Number(pass.finalized_at) || startedAt;
    let group = passGroups.at(-1);
    if (!group || group.identity !== identity || startedAt > group.finalized_at + 30000)
      group = null;
    if (!group) { group = {identity, started_at: startedAt, finalized_at: finalizedAt,
      nodes: [], passes: []}; passGroups.push(group); }
    group.started_at = Math.min(group.started_at, startedAt);
    group.finalized_at = Math.max(group.finalized_at, finalizedAt);
    group.nodes.push(node.id); group.passes.push(pass);
  }
  const legacyUniquePasses = passGroups.map(group => ({identity: group.identity,
    started_at: Math.min(...group.passes.map(pass => Number(pass.started_at))),
    finalized_at: Math.max(...group.passes.map(pass => Number(pass.finalized_at) || 0)),
    nodes: [...new Set(group.nodes)].sort(),
    channels: [...new Set(group.passes.map(pass => pass.channel).filter(Boolean))].sort(),
    eligible: group.passes.some(pass => pass.eligible === true || pass.eligible_cpis >= 3),
    detected: group.passes.some(pass => pass.detected === true ||
      ['detected_not_tracked', 'tracked'].includes(pass.outcome)),
    tracked: group.passes.some(pass => pass.tracked === true || pass.outcome === 'tracked')}));
  void legacyUniquePasses;
  const uniquePasses = deduplicatePasses(available);
  const eligibleUnique = uniquePasses.filter(pass => pass.eligible);
  const detectedUnique = eligibleUnique.filter(pass => pass.detected);
  const trackedUnique = detectedUnique.filter(pass => pass.tracked);
  const systemWide = {scope: 'all-nodes-all-channels-deduplicated',
    window: 'common-retained-finalized-passes',
    window_started_at: alignment.commonStart,
    eligible_adsb_passes: eligibleUnique.length,
    detection: {associated_passes: detectedUnique.length,
      eligible_adsb_passes: eligibleUnique.length,
      yield: eligibleUnique.length ? detectedUnique.length / eligibleUnique.length : null},
    tracking: {associated_passes: trackedUnique.length,
      detected_passes: detectedUnique.length,
      yield: detectedUnique.length ? trackedUnique.length / detectedUnique.length : null,
      tracked_of_eligible_yield: eligibleUnique.length ?
        trackedUnique.length / eligibleUnique.length : null},
    missed_detections: eligibleUnique.length - detectedUnique.length,
    detected_not_tracked: detectedUnique.length - trackedUnique.length,
    passes: uniquePasses.sort((a, b) => b.started_at - a.started_at).slice(0, 100)};
  const cooperativeTracks = cooperative?.available ? cooperative.tracks || [] : [];
  const unhealthy = nodes.filter(node => !node?.available || node.radar !== 'receiving' ||
    node.processor_fresh !== true || node.config_match !== true || node.tracking_status === 'degraded' ||
    (node.timing?.cadence_ms > 0 && node.timing?.cpi_ms > node.timing.cadence_ms * 1.1) ||
    (node.timing?.cadence_ms > 0 && node.timing?.backlog_ms > node.timing.cadence_ms * 1.25));
  return {schema: 'vectorwarp-network-statistics-v1', scope: 'all-nodes',
    updated_at: Date.now(), statistics_window: {basis: 'common-retained-finalized-passes',
      started_at: alignment.commonStart},
    health: {status: unhealthy.length ? 'degraded' : 'healthy',
      nodes_total: nodes.length, nodes_available: available.length,
      unhealthy_nodes: unhealthy.map(node => node?.id).filter(Boolean),
      maximum_cpi_ms: Math.max(0, ...available.map(node => node.timing?.cpi_ms || 0)),
      maximum_backlog_ms: Math.max(0, ...available.map(node => node.timing?.backlog_ms || 0)),
      capture_drops: available.reduce((sum, node) => sum + (node.timing?.drops || []).reduce((a, b) => a + Number(b || 0), 0), 0),
      retired_cpis: available.reduce((sum, node) => sum + Number(node.timing?.retired_cpis || 0), 0),
      retired_samples: available.reduce((sum, node) => sum + Number(node.timing?.retired_samples || 0), 0)},
    channel_opportunities: {...association,
      detection_yield: association.eligible ? association.detected / association.eligible : null,
      tracking_yield: association.detected ? association.tracked / association.detected : null,
      tracked_of_eligible_yield: association.eligible ? association.tracked / association.eligible : null},
    system_wide_unique_passes: systemWide,
    unique_recent_passes: {eligible: systemWide.eligible_adsb_passes,
      detected: systemWide.detection.associated_passes,
      tracked: systemWide.tracking.associated_passes,
      detection_yield: systemWide.detection.yield,
      tracking_yield: systemWide.tracking.yield,
      passes: systemWide.passes},
    cooperative_tracks: {active: cooperativeTracks.filter(track => track.state === 'COOPERATIVE_ACTIVE').length,
      coasting: cooperativeTracks.filter(track => track.state === 'COOPERATIVE_COASTING').length,
      candidates: cooperativeTracks.filter(track => track.state === 'COOPERATIVE_CANDIDATE').length},
    nodes: available, channels};
}

function installNodeFederation(app, filename = defaultRegistryPath(),
    {config = {}} = {}) {
  let nodes = registry(filename);
  const hostnameIdentity = os.hostname().toLowerCase().replace(/[^a-z0-9_-]+/g, '-')
    .replace(/^-+|-+$/g, '').slice(0, 32);
  const identity = process.env.VECTORWARP_NODE_ID || hostnameIdentity || 'vectorwarp-node';
  const localLabel = process.env.VECTORWARP_NODE_LABEL || 'Local';
  if (identity && !/^[a-z0-9][a-z0-9_-]{0,63}$/.test(identity)) throw new Error('Invalid node identity');
  const trackerConfig = config?.process?.tracker || {};
  const cooperativeTracker = new CooperativeDetectionTracker({
    cpiSeconds: Number(config?.process?.data?.cpi) || .5,
    m: Number(trackerConfig?.initiate?.M) || 4,
    n: Number(trackerConfig?.initiate?.N) || 6,
    deleteAfter: Number(trackerConfig?.delete) || 3,
    maximumAccelerationHzS: Number(trackerConfig?.initiate?.maxAcc) || 10,
    coLocationRadiusM: Number(process.env.VECTORWARP_COOPERATIVE_COLOCATION_M) || 100
  });
  const cooperativeConfigCache = new Map([[identity, {value: config, updatedAt: Date.now()}]]);
  let cooperativeBusy = false, cooperativeLastPollAt = null, cooperativeLastError = null;
  let networkStatisticsCache = null, networkStatisticsCacheAt = 0;
  let networkStatisticsPromise = null;
  let cooperativeTotalSourceCount = 0, cooperativeSourceCount = 0, cooperativeAvailableSources = 0;
  const configuredBindAddress = net.isIP(config?.network?.ip || '') ? config.network.ip : '0.0.0.0';
  const localAddress = configuredBindAddress === '0.0.0.0' ? '127.0.0.1' :
    configuredBindAddress === '::' ? '::1' : configuredBindAddress;
  const localOrigin = `http://${net.isIP(localAddress) === 6 ? `[${localAddress}]` : localAddress}:${configuredApiPort(config)}`;
  const cooperativeEntries = () => [{id: identity, label: localLabel,
    expectedIdentity: identity, origin: localOrigin, local: true}, ...nodes.map(node => ({id: node.id,
    label: node.label, expectedIdentity: node.expectedIdentity, origin: node.origin, local: false}))];
  async function cooperativeRead(entry, pathname, timeoutMs = 900) {
    const result = await probeJson(entry.origin, pathname, timeoutMs);
    if (result.identity !== entry.expectedIdentity) throw new Error(`Node identity mismatch: ${entry.id}`);
    return result.value;
  }
  async function cooperativeConfig(entry) {
    const cached = cooperativeConfigCache.get(entry.id);
    if (cached && (entry.local || Date.now() - cached.updatedAt < 60000)) return cached.value;
    const value = await cooperativeRead(entry, '/api/runtime/config', 1500);
    cooperativeConfigCache.set(entry.id, {value, updatedAt: Date.now()});
    return value;
  }
  async function cooperativeTick() {
    if (cooperativeBusy) return;
    cooperativeBusy = true;
    try {
      const entries = cooperativeEntries();
      const configurations = await Promise.all(entries.map(async entry => {
        try { return {entry, config: await cooperativeConfig(entry)}; }
        catch (_) { return {entry, config: null}; }
      }));
      const configuredSources = configurations.flatMap(({entry, config: value}) => value ?
        cooperativeSources(entry, value).map(source => ({...source, origin: entry.origin,
          expectedIdentity: entry.expectedIdentity})) : []);
      cooperativeTotalSourceCount = configuredSources.length;
      const catalog = geometryCatalog(configuredSources,
        Number(process.env.VECTORWARP_COOPERATIVE_COLOCATION_M) || 100);
      const nodesByGeometry = new Map();
      for (const source of configuredSources) {
        const geometry = catalog.get(source.key);
        if (!geometry) continue;
        if (!nodesByGeometry.has(geometry)) nodesByGeometry.set(geometry, new Set());
        nodesByGeometry.get(geometry).add(source.node);
      }
      // Raw bins can cooperate only when another physical node observes the
      // same receiver/transmitter geometry. Distinct geometry goes through the
      // geographic solver instead of being silently mixed here.
      const sources = configuredSources.filter(source =>
        (nodesByGeometry.get(catalog.get(source.key))?.size || 0) >= 2);
      cooperativeSourceCount = sources.length;
      const frames = (await Promise.all(sources.map(async source => {
        try {
          const value = await cooperativeRead({id: source.node, origin: source.origin,
            expectedIdentity: source.expectedIdentity}, source.detection_path);
          return {source: source.key, frame: value};
        } catch (_) { return null; }
      }))).filter(Boolean);
      cooperativeAvailableSources = frames.length;
      if (frames.length) cooperativeTracker.update(sources, frames);
      cooperativeLastPollAt = Date.now(); cooperativeLastError = null;
    } catch (error) {
      cooperativeLastPollAt = Date.now(); cooperativeLastError = error.message;
    } finally { cooperativeBusy = false; }
  }
  app.use((_req, res, next) => { if (identity) res.set('X-VectorWarp-Node-Id', identity); next(); });
  app.get('/api/federation/identity', (_req, res) => res.json({identity: identity || null,
    label: localLabel}));
  app.get('/api/federation/capabilities', (_req, res) => res.json(nodeCapabilities(config)));
  let active = 0;
  let discoveries = 0;
  app.get('/api/federation/nodes', (_req, res) => res.json({local_only: true, nodes: [
    {id: 'local', identity: identity || null, label: localLabel, local: true},
    ...nodes.map(({id, label}) => ({id, identity: id, label, local: false}))]}));
  app.get('/api/federation/cooperative', (_req, res) => res.json({
    ...cooperativeTracker.snapshot(), coordinator: identity,
    polling: {last_poll_at: cooperativeLastPollAt, busy: cooperativeBusy,
      total_sources: cooperativeTotalSourceCount, shared_geometry_sources: cooperativeSourceCount,
      available_sources: cooperativeAvailableSources,
      error: cooperativeLastError}}));
  app.get('/api/federation/statistics-summary', async (_req, res) => {
    try {
      if (networkStatisticsCache && Date.now() - networkStatisticsCacheAt < 2000)
        return res.json(networkStatisticsCache);
      if (!networkStatisticsPromise) networkStatisticsPromise = (async () => {
        const rows = await Promise.all(cooperativeEntries().map(async entry => {
          try {
            const [status, timing, scorecard, associationPasses] = await Promise.all([
              cooperativeRead(entry, '/api/system/status', 2500),
              cooperativeRead(entry, '/api/timing', 2500),
              cooperativeRead(entry,
                '/api/live-scorecard/association-basis?timing=provisional', 3500),
              cooperativeRead(entry,
                '/api/association/passes?timing=provisional', 3500)]);
            return compactNodeStatistics(entry, status, timing, scorecard,
              associationPasses);
          } catch (error) { return {id: entry.id, label: entry.label, local: entry.local,
            available: false, error: error.message, channels: [], passes: []}; }
        }));
        return combineNodeStatistics(rows, cooperativeTracker.snapshot());
      })();
      networkStatisticsCache = await networkStatisticsPromise;
      networkStatisticsCacheAt = Date.now();
      res.json(networkStatisticsCache);
    } finally { networkStatisticsPromise = null; }
  });
  app.post('/api/federation/nodes', async (req, res) => {
    if (req.get('X-VectorWarp-Intent') !== 'local-node-add-v1')
      return res.status(428).json({error: 'Local-node intent header required'});
    if (discoveries >= 2) return res.status(503).json({error: 'Node discovery is busy'});
    discoveries++;
    try {
      if (nodes.length >= 16) return res.status(409).json({error: 'The local node limit is 16'});
      const discovered = await discoverLocalNode(req.body?.host, req.body?.port, req.body?.label);
      // Discovery awaits remote I/O. Recheck the live registry immediately
      // before the synchronous atomic save so concurrent additions cannot
      // exceed the bound or publish a duplicate identity.
      if (nodes.length >= 16) return res.status(409).json({error: 'The local node limit is 16'});
      if (discovered.id === identity || nodes.some(node => node.id === discovered.id))
        return res.status(409).json({error: 'That VectorWarp node is already connected'});
      const updated = [...nodes, {...discovered, origin: discovered.url, token: null,
        stored: discovered}];
      saveRegistry(filename, updated);
      nodes = updated;
      res.status(201).json({ok: true, node: {id: discovered.id, label: discovered.label,
        local: false}});
    } catch (error) { res.status(400).json({error: error.message}); }
    finally { discoveries--; }
  });
  app.delete('/api/federation/nodes/:id', (req, res) => {
    if (req.get('X-VectorWarp-Intent') !== 'local-node-remove-v1')
      return res.status(428).json({error: 'Local-node intent header required'});
    const kept = nodes.filter(node => node.id !== req.params.id);
    if (kept.length === nodes.length) return res.status(404).json({error: 'Node not found'});
    try { saveRegistry(filename, kept); nodes = kept; res.json({ok: true}); }
    catch (error) { res.status(500).json({error: 'The node registry could not be saved'}); }
  });
  const activeByNode = new Map();
  let aggregateActive = 0;
  app.get('/api/federation/aggregate', async (req, res) => {
    if (aggregateActive >= 2) return res.status(503).json({error: 'Aggregate busy'});
    aggregateActive++;
    const entries = [{id: 'local', label: localLabel, local: true},
      ...nodes.map(({id, label}) => ({id, label, local: false}))];
    const host = req.socket.localAddress;
    const port = req.socket.localPort;
    const pending = new Set();
    let cancelled = false, timedOut = false;
    const cancel = () => {
      cancelled = true;
      for (const request of pending) request.destroy(new Error('Aggregate cancelled'));
      pending.clear();
    };
    res.once('close', () => { if (!res.writableEnded) cancel(); });
    const aggregateDeadline = setTimeout(() => { timedOut = true; cancel(); }, 8000);
    const read = target => new Promise(resolve => {
      if (cancelled) return resolve(null);
      let done = false, size = 0, request; const chunks = [];
      const finish = value => { if (!done) { done = true; clearTimeout(deadline);
        if (request) pending.delete(request); resolve(value); } };
      request = http.get({host, port, path: target, timeout: 2500}, response => {
        if (response.statusCode !== 200) { response.resume(); return finish(null); }
        response.on('data', chunk => { size += chunk.length; if (size > 512 * 1024) response.destroy(); else chunks.push(chunk); });
        response.on('end', () => { try { finish(JSON.parse(Buffer.concat(chunks))); } catch (_) { finish(null); } });
        response.on('error', () => finish(null));
      });
      pending.add(request);
      const deadline = setTimeout(() => { request.destroy(); finish(null); }, 2500);
      request.on('error', () => finish(null));
    });
    let cursor = 0;
    const result = new Array(entries.length);
    async function worker() {
      while (cursor < entries.length) {
        const index = cursor++, entry = entries[index];
        const prefix = entry.local ? '' : `/api/federation/nodes/${encodeURIComponent(entry.id)}`;
        const status = await read(prefix + '/api/system/status?summary=1');
        if (status === null || cancelled) {
          result[index] = {...entry, available: false, status: null,
            layers: {locations: null, tracks: null}};
          continue;
        }
        const capabilities = await read(prefix + '/api/federation/capabilities');
        const supports = resource => !capabilities ||
          capabilities.schema !== 'vectorwarp-node-capabilities-v1' ||
          capabilities.resources?.includes(resource);
        const locations = supports('map') ? await read(prefix + '/api/locations') : null;
        const tracks = supports('tracks') ? await read(prefix + '/api/tracker') : null;
        result[index] = {...entry, available: true, status,
          capabilities, layers: {locations, tracks}};
      }
    }
    try { await Promise.all(Array.from({length: Math.min(2, entries.length)}, worker));
      if (!res.writableEnded && !res.destroyed) {
        res.json({nodes: result, timedOut,
          cooperative: cooperativeTracker.snapshot()});
      }
    } finally { clearTimeout(aggregateDeadline); cancel(); aggregateActive--; }
  });
  app.all('/api/federation/nodes/:id/*', (req, res) => {
    if (req.method !== 'GET') return res.status(405).json({error: 'Read only'});
    const node = nodes.find(item => item.id === req.params.id);
    const target = '/' + req.params[0];
    const recording = /^\/api\/recordings\/([A-Za-z0-9_-]{1,256})\/(file|package|replay-profile)$/.exec(target);
    const streaming = recording && recording[2] !== 'replay-profile';
    if (!node || (!READS.has(target) && !recording)) return res.status(404).json({error: 'Not available'});
    const range = req.get('Range');
    if (range && (!streaming || !/^bytes=\d{1,15}-\d{1,15}$/.test(range))) return res.status(416).json({error: 'Bounded range required'});
    if (range) { const [start, end] = range.slice(6).split('-').map(Number);
      if (!Number.isSafeInteger(start) || !Number.isSafeInteger(end) || end < start || end - start + 1 > 64 * 1024 * 1024)
        return res.status(416).json({error: 'Range too large'}); }
    if (active >= MAX_ACTIVE) return res.status(503).json({error: 'Federation busy'});
    if ((activeByNode.get(node.id) || 0) >= 4)
      return res.status(503).json({error: 'Node busy'});
    const query = req.originalUrl.includes('?') ? req.originalUrl.slice(req.originalUrl.indexOf('?')) : '';
    if (query.length > 2048) return res.status(400).json({error: 'Query too long'});
    active++;
    activeByNode.set(node.id, (activeByNode.get(node.id) || 0) + 1);
    const upstream = new URL(target + query, node.origin);
    const headers = {'Accept-Encoding': 'identity', ...(node.token ? {Authorization: `Bearer ${node.token}`} : {}), ...(range ? {Range: range} : {})};
    const request = (upstream.protocol === 'https:' ? https : http).get(upstream, {headers, timeout: 5000}, response => {
      if (response.headers['x-vectorwarp-node-id'] !== node.expectedIdentity) {
        response.destroy(); return res.status(502).json({error: 'Node identity mismatch'});
      }
      if (response.statusCode >= 300 && response.statusCode < 400) {
        response.destroy(); return res.status(502).json({error: 'Redirect rejected'});
      }
      if (streaming) {
        const length = Number(response.headers['content-length']);
        const contentRange = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers['content-range'] || '');
        const requested = range && range.slice(6).split('-').map(Number);
        if (![200, 206].includes(response.statusCode) || !Number.isSafeInteger(length) || length < 0 || length > 64 * 1024 * 1024 ||
            (range && (response.statusCode !== 206 || !contentRange || Number(contentRange[1]) !== requested[0] ||
              Number(contentRange[2]) > requested[1] || Number(contentRange[2]) - Number(contentRange[1]) + 1 !== length ||
              Number(contentRange[3]) <= Number(contentRange[2])))) {
          response.destroy();
          return res.status(502).json({error: 'Recording response exceeds limits or lacks range support'});
        }
        res.status(response.statusCode).set('Content-Type', recording[2] === 'file' ? 'application/octet-stream' : 'application/vnd.vectorwarp.recording');
        res.set('Content-Disposition', `attachment; filename="${node.id}-${recording[1]}.${recording[2] === 'file' ? 'blah2iq' : 'vwr'}"`);
        res.set('Content-Length', String(length));
        if (response.headers['content-range'] && /^bytes \d+-\d+\/\d+$/.test(response.headers['content-range'])) res.set('Content-Range', response.headers['content-range']);
        if (response.headers['accept-ranges'] === 'bytes') res.set('Accept-Ranges', 'bytes');
        let sent = 0; response.on('data', chunk => { sent += chunk.length; if (sent > length) response.destroy(); });
        response.on('error', () => res.destroy());
        return response.pipe(res);
      }
      let size = 0; const chunks = [];
      response.on('data', chunk => { size += chunk.length; if (size > MAX_BYTES) response.destroy(new Error('Response too large')); else chunks.push(chunk); });
      response.on('end', () => {
        if (res.writableEnded) return;
        let body = Buffer.concat(chunks);
        if (target === '/api/runtime/config' && response.statusCode === 200) {
          try { body = Buffer.from(JSON.stringify(safeConfig(JSON.parse(body)))); }
          catch (_) { return res.status(502).json({error: 'Invalid upstream response'}); }
        }
        if (recording && recording[2] === 'replay-profile' && response.statusCode === 200) {
          try { const value = JSON.parse(body); body = Buffer.from(JSON.stringify({nodeId: node.id, recordingId: recording[1],
            sample_rate_hz: value.sample_rate_hz, frequency_hz: value.frequency_hz, channel_count: value.channel_count,
            reference_mode: value.reference_mode, limitations: value.limitations})); }
          catch (_) { return res.status(502).json({error: 'Invalid recording detail'}); }
        }
        res.status(response.statusCode || 502).type('application/json').send(body);
      });
      response.on('error', () => { if (!res.headersSent) res.status(502).json({error: 'Node unavailable'}); });
    });
    const deadline = setTimeout(() => request.destroy(new Error('Deadline exceeded')), streaming ? 60000 : 5000);
    request.on('timeout', () => request.destroy(new Error('Timeout')));
    request.on('error', () => { if (!res.headersSent) res.status(502).json({error: 'Node unavailable'}); });
    request.on('close', () => { clearTimeout(deadline); active--;
      const remaining = (activeByNode.get(node.id) || 1) - 1;
      if (remaining > 0) activeByNode.set(node.id, remaining);
      else activeByNode.delete(node.id);
    });
    res.on('close', () => { if (!res.writableEnded) request.destroy(); });
  });
  const cooperativeIntervalMs = Math.max(250, Math.min(2000,
    Math.round((Number(config?.process?.data?.cpi) || .5) * 1000)));
  const cooperativeInterval = setInterval(cooperativeTick, cooperativeIntervalMs);
  cooperativeInterval.unref();
  const cooperativeInitial = setTimeout(cooperativeTick, 1000);
  cooperativeInitial.unref();
}
module.exports = {installNodeFederation, registry, safeConfig, nodeCapabilities, READS,
  localNodeOrigin, discoverLocalNode, cooperativeSources, compactNodeStatistics,
  combineNodeStatistics, deduplicatePasses, alignNodeStatistics, defaultRegistryPath,
  configuredApiPort};
