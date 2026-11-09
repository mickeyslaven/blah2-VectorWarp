'use strict';

const path = require('path');
const {createEvidenceStatisticsLanes, provisionalTimingFrame} =
  require('./evidence-statistics');
const {DelayDopplerHistory} = require('./adsb-geometry');
const {effectiveDelayConfig} = require('../html/js/config_profiles');

const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
const safeId = value => String(value).replace(/[^A-Za-z0-9._-]+/g, '_');

function enabledIlluminators(config) {
  if (config.process?.multi_illuminator?.enabled !== true) return [];
  return (config.process.multi_illuminator.illuminators || [])
    .filter(item => item?.enabled === true);
}

function laneConfig(config, illuminator) {
  const value = clone(config);
  value.capture.fc = illuminator.frequency_hz;
  value.capture.fs = illuminator.output_sample_rate_hz;
  value.location ||= {};
  value.location.tx = clone(illuminator.transmitter);
  return effectiveDelayConfig(value, illuminator.output_sample_rate_hz,
    {scaleNeighborhoods: true});
}

function statisticsFilename(base, id, timing) {
  const parsed = path.parse(base);
  return path.join(parsed.dir, `${parsed.name}-${safeId(id)}-${timing}${parsed.ext || '.json'}`);
}

function identity(illuminator, statistics) {
  return {illuminator: illuminator.id, standard: illuminator.standard || null,
    frequency_hz: illuminator.frequency_hz,
    sample_rate_hz: illuminator.output_sample_rate_hz,
    transmitter: clone(illuminator.transmitter) || null,
    site_epoch: statistics.key, config_epoch: statistics.configEpoch,
    receiver_epoch: statistics.receiverEpoch};
}

function associationStatistics(value) {
  const window = value?.current_setup_lifetime || value?.windows?.lifetime || {};
  const eligible = Number(window.eligible_passes) || 0;
  const detected = Number(window.detected_passes) || 0;
  const tracked = Number(window.tracked_passes) || 0;
  const audited = Number(value?.data_quality?.audited_aircraft_cpis) || 0;
  const exactTimed = Number(value?.data_quality?.exact_timed_aircraft_cpis) || 0;
  return {
    scope: 'current-setup-lifetime',
    denominator_semantics: window.denominator_semantics ||
      'finalized passes with >=3 detector-searchable, fresh-truth CPIs',
    eligible_adsb_passes: eligible,
    detection: {associated_passes: detected, eligible_adsb_passes: eligible,
      yield: eligible ? detected / eligible : null},
    tracking: {associated_passes: tracked, detected_passes: detected,
      yield: detected ? tracked / detected : null,
      tracked_of_eligible_yield: eligible ? tracked / eligible : null},
    timing: {exact_timed_aircraft_cpis: exactTimed, audited_aircraft_cpis: audited,
      exact_fraction: audited ? exactTimed / audited : null}
  };
}

function deduplicatedAssociationStatistics(passSets = [], directionalPassSets = passSets,
    channelIds = []) {
  const passes = new Map();
  for (const [index, channelPasses] of passSets.entries()) {
    const channel = channelIds[index] || `channel-${index + 1}`;
    for (const pass of channelPasses || []) {
      if (!pass?.hex || !Number.isFinite(pass.started_at)) continue;
      const key = `${String(pass.hex).toLowerCase()}:${pass.started_at}`;
      const prior = passes.get(key) || {detected: false, tracked: false, channels: {},
        azimuth_bands: {}, azimuth_elevation_bands: {}};
      prior.channels[channel] = {eligible: true, detected: pass.detected === true,
        tracked: pass.tracked === true};
      prior.detected ||= pass.detected === true;
      prior.tracked ||= pass.tracked === true;
      for (const field of ['azimuth_bands', 'azimuth_elevation_bands'])
        for (const [id, band] of Object.entries(pass[field] || {})) {
          const merged = prior[field][id] ||= {...band, eligible: false,
            detected: false, tracked: false};
          merged.eligible ||= band.eligible === true;
          merged.detected ||= band.detected === true;
          merged.tracked ||= band.tracked === true;
        }
      passes.set(key, prior);
    }
  }
  const eligible = passes.size;
  const detected = [...passes.values()].filter(pass => pass.detected).length;
  const tracked = [...passes.values()].filter(pass => pass.tracked).length;
  const contribution = metric => {
    const onlyByChannel = Object.fromEntries(channelIds.map(id => [id, 0]));
    let shared = 0, uncaught = 0;
    for (const pass of passes.values()) {
      const caught = Object.entries(pass.channels || {}).filter(([, value]) =>
        value[metric] === true).map(([id]) => id);
      if (!caught.length) uncaught += 1;
      else if (caught.length > 1) shared += 1;
      else onlyByChannel[caught[0]] = (onlyByChannel[caught[0]] || 0) + 1;
    }
    return {shared_by_multiple_channels: shared, only_by_channel: onlyByChannel,
      not_caught: uncaught};
  };
  const channelContribution = channelIds.map(id => {
    const rows = [...passes.values()].map(pass => pass.channels?.[id]).filter(Boolean);
    const detectionOnly = [...passes.values()].filter(pass =>
      pass.channels?.[id]?.detected === true && Object.entries(pass.channels)
        .every(([other, value]) => other === id || value.detected !== true)).length;
    const trackingOnly = [...passes.values()].filter(pass =>
      pass.channels?.[id]?.tracked === true && Object.entries(pass.channels)
        .every(([other, value]) => other === id || value.tracked !== true)).length;
    return {channel: id, eligible_passes: rows.length,
      eligible_only_on_channel: [...passes.values()].filter(pass =>
        pass.channels?.[id] && Object.keys(pass.channels).length === 1).length,
      detected_passes: rows.filter(row => row.detected).length,
      tracked_passes: rows.filter(row => row.tracked).length,
      unique_detection_contribution: detectionOnly,
      unique_tracking_contribution: trackingOnly,
      leave_one_out_detection_loss: detectionOnly,
      leave_one_out_tracking_loss: trackingOnly};
  });
  const directionalPasses = directionalPassSets === passSets ? passes : new Map();
  if (directionalPassSets !== passSets)
    for (const channelPasses of directionalPassSets) for (const pass of channelPasses || []) {
      if (!pass?.hex || !Number.isFinite(pass.started_at)) continue;
      const key = `${String(pass.hex).toLowerCase()}:${pass.started_at}`;
      const prior = directionalPasses.get(key) || {azimuth_bands: {},
        azimuth_elevation_bands: {}};
      for (const field of ['azimuth_bands', 'azimuth_elevation_bands'])
        for (const [id, band] of Object.entries(pass[field] || {})) {
          const merged = prior[field][id] ||= {...band, eligible: false,
            detected: false, tracked: false};
          merged.eligible ||= band.eligible === true;
          merged.detected ||= band.detected === true;
          merged.tracked ||= band.tracked === true;
        }
      directionalPasses.set(key, prior);
    }
  const summarizeBands = field => {
    const bands = {};
    for (const pass of directionalPasses.values())
      for (const [id, band] of Object.entries(pass[field] || {})) {
        const value = bands[id] ||= {...band, eligible_passes: 0,
          detected_passes: 0, tracked_passes: 0};
        if (!band.eligible) continue;
        value.eligible_passes += 1;
        if (band.detected) value.detected_passes += 1;
        if (band.tracked) value.tracked_passes += 1;
      }
    for (const value of Object.values(bands)) {
      value.detection_yield = value.eligible_passes ?
        value.detected_passes / value.eligible_passes : null;
      value.tracking_yield = value.eligible_passes ?
        value.tracked_passes / value.eligible_passes : null;
      delete value.eligible; delete value.detected; delete value.tracked;
    }
    return bands;
  };
  return {scope: 'unique-aircraft-passes-caught-by-any-channel',
    aggregation: 'deduplicated-aircraft-pass-union',
    cross_channel_aircraft_deduplicated: true,
    denominator_semantics: 'A finalized eligible ADS-B pass is counted once across all channels; detection or tracking succeeds when any channel succeeds.',
    eligible_adsb_passes: eligible,
    detection: {associated_passes: detected, eligible_adsb_passes: eligible,
      yield: eligible ? detected / eligible : null},
    tracking: {associated_passes: tracked, detected_passes: detected,
      yield: detected ? tracked / detected : null,
      tracked_of_eligible_yield: eligible ? tracked / eligible : null},
    channel_contribution: {scope: 'retained-finalized-aircraft-pass-identities',
      denominator_semantics: 'Every retained finalized aircraft pass is counted once. Shared means two or more channels succeeded; unique contribution is the union loss if that channel is removed.',
      eligible_aircraft_passes: eligible, detection: contribution('detected'),
      tracking: contribution('tracked'), channels: channelContribution},
    directional_coverage: {
      scope: 'deduplicated-aircraft-pass-union-by-receiver-direction',
      window: 'past-24-hours-finalized-passes',
      includes_active_passes: false,
      denominator_semantics: 'Within each direction cell over the finalized past 24 hours, an eligible aircraft pass is counted once across all channels and succeeds when any channel succeeds.',
      azimuth_strata: {definition: 'receiver_true_azimuth_deg',
        bands: summarizeBands('azimuth_bands')},
      azimuth_elevation_strata: {
        definition: 'receiver_true_azimuth_x_geometric_elevation',
        bands: summarizeBands('azimuth_elevation_bands')}}};
}

function aggregateTrackPopulations(channels) {
  const populations = channels.map(channel => ({channel:
    channel.identity?.illuminator || channel.selected_channel,
  ...(channel.track_population || {})}));
  const sum = field => populations.reduce((total, item) =>
    total + (Number(item[field]) || 0), 0);
  const bands = {'high-similarity': 0, 'moderate-similarity': 0,
    'low-similarity': 0, 'insufficient-reference': 0, 'insufficient-evidence': 0};
  for (const population of populations)
    for (const name of Object.keys(bands))
      bands[name] += Number(population.similarity_bands?.[name]) || 0;
  const candidates = populations.flatMap(population =>
    (population.aircraft_like_candidates || []).map(track =>
      ({...track, channel: population.channel})))
    .sort((left, right) => (right.similarity?.score || 0) -
      (left.similarity?.score || 0) || right.last_at - left.last_at).slice(0, 50);
  return {schema: 'vectorwarp-track-population-aggregate-v1',
    scope: 'sum-of-channel-track-cohorts', retained_tracks: sum('retained_tracks'),
    adsb_associated_tracks: sum('adsb_associated_tracks'),
    unassociated_tracks: sum('unassociated_tracks'),
    empirical_reference_tracks: sum('empirical_reference_tracks'),
    similarity_bands: bands, aircraft_like_candidates: candidates,
    channels: populations.map(population => ({channel: population.channel,
      retained_tracks: population.retained_tracks || 0,
      adsb_associated_tracks: population.adsb_associated_tracks || 0,
      unassociated_tracks: population.unassociated_tracks || 0,
      similarity_bands: population.similarity_bands || {}}))};
}

function aggregateAssociationStatistics(channels, passSets = [], directionalPassSets = passSets) {
  const sum = (path, fallback = 0) => channels.reduce((total, channel) => {
    let value = channel.association_statistics;
    for (const key of path) value = value?.[key];
    return total + (Number(value) || fallback);
  }, 0);
  const eligible = sum(['eligible_adsb_passes']);
  const detected = sum(['detection', 'associated_passes']);
  const tracked = sum(['tracking', 'associated_passes']);
  const audited = sum(['timing', 'audited_aircraft_cpis']);
  const exactTimed = sum(['timing', 'exact_timed_aircraft_cpis']);
  return {scope: 'all-illuminator-channel-opportunities',
    aggregation: 'sum-of-independent-channel-opportunities',
    cross_channel_aircraft_deduplicated: false,
    denominator_semantics: 'Sum of each channel\'s eligible ADS-B pass denominator; one aircraft pass can contribute once per channel.',
    eligible_adsb_passes: eligible,
    detection: {associated_passes: detected, eligible_adsb_passes: eligible,
      yield: eligible ? detected / eligible : null},
    tracking: {associated_passes: tracked, detected_passes: detected,
      yield: detected ? tracked / detected : null,
      tracked_of_eligible_yield: eligible ? tracked / eligible : null},
    timing: {exact_timed_aircraft_cpis: exactTimed, audited_aircraft_cpis: audited,
      exact_fraction: audited ? exactTimed / audited : null},
    track_population: aggregateTrackPopulations(channels),
    unique_aircraft_passes: deduplicatedAssociationStatistics(passSets, directionalPassSets,
      channels.map(channel => channel.identity?.illuminator || channel.selected_channel))};
}

function laneAdsbSource(config, source) {
  const geometry = new DelayDopplerHistory(config);
  const aircraft = async (method, timestamp) => {
    if (method === 'getAt' && typeof source.getAt === 'function')
      return source.getAt('aircraft', timestamp);
    return source.get('aircraft');
  };
  return {
    get: async kind => kind === 'delayDoppler' ?
      geometry.update(await aircraft('get')).data : source.get(kind),
    ...(typeof source.getAt === 'function' ? {getAt: async (kind, timestamp) =>
      kind === 'delayDoppler' ? geometry.update(await aircraft('getAt', timestamp)).data :
        source.getAt(kind, timestamp)} : {}),
    status: (...args) => source.status(...args)
  };
}

// The legacy map/detection/tracker sockets mirror the first enabled lane.
// Their consumers need its radar units and truth geometry; raw recording and
// receiver operations must continue to use the full-passband capture config.
function primaryRadarContext(config, source) {
  const illuminator = enabledIlluminators(config)[0];
  if (!illuminator) return {config, source, illuminator: null};
  const primary = laneConfig(config, illuminator);
  return {config: primary, source: laneAdsbSource(primary, source), illuminator};
}

function captureDependencies(illuminator, dependencies) {
  const normalize = event => {
    const aircraft = String(event?.hex || event?.flight || event?.pass_id || 'unknown');
    const observed = Number.isFinite(event?.observed_at) ? event.observed_at : 0;
    // Keep a distinct queue parent for each lane. The queue already coalesces
    // overlapping requests into one physical full-passband clip; reusing one
    // parent ID here instead makes every lane after the first look like a
    // duplicate and leaves its evidence ledger permanently not_integrated.
    // The bounded pass bucket still gives the physical requests stable IDs and
    // permits a genuinely later pass by the same aircraft to record again.
    const passBucket = Math.floor(observed / (15 * 60 * 1000));
    const id = `eligibility:multi:${aircraft}:${passBucket}:${safeId(illuminator.id)}`;
    return {...event, id, pass_id: id, illuminator: illuminator.id,
      lane_event_id: event?.id || null, lane_pass_id: event?.pass_id || null};
  };
  const invoke = (callback, event, timestamp) => {
    if (typeof callback !== 'function') return undefined;
    const normalized = normalize(event);
    const result = timestamp === undefined ? callback(normalized) : callback(normalized, timestamp);
    const captureId = result?.clip?.captureIds?.[normalized.id];
    if (captureId && event?.id && result.clip.captureIds[event.id] === undefined)
      result.clip.captureIds[event.id] = captureId;
    return result;
  };
  return {
    onEligibilityEvent: event => invoke(dependencies.onEligibilityEvent, event),
    onPeriodicCapture: (event, timestamp) =>
      invoke(dependencies.onPeriodicCapture, event, timestamp),
    onTrackCapture: (event, timestamp) =>
      invoke(dependencies.onTrackCapture, event, timestamp),
    onUnmatchedMotionCapture: (event, timestamp) => {
      if (typeof dependencies.onUnmatchedMotionCapture !== 'function') return undefined;
      const id = `unmatched-motion:multi:${safeId(illuminator.id)}:${safeId(event.track_id)}`;
      return dependencies.onUnmatchedMotionCapture({...event, id, pass_id: id,
        illuminator: illuminator.id, lane_event_id: event.id}, timestamp);
    }
  };
}

class MultiIlluminatorEvidence {
  constructor(config, adsbSource, {baseFilename = process.env.BLAH2_STATISTICS_PATH ||
    '/var/lib/vectorwarp-api/evidence-statistics.json', createLanes = createEvidenceStatisticsLanes,
    dependencies = {}} = {}) {
    this.captureSampleRateHz = Number(config.capture?.fs);
    this.lanes = new Map();
    for (const illuminator of enabledIlluminators(config)) {
      const lane = laneConfig(config, illuminator);
      const capture = captureDependencies(illuminator, dependencies);
      const created = createLanes(lane, laneAdsbSource(lane, adsbSource), {
        exact: {...dependencies,
          filename: statisticsFilename(baseFilename, illuminator.id, 'exact')},
        operational: {...dependencies,
          filename: statisticsFilename(baseFilename, illuminator.id, 'provisional'),
          ...capture}});
      this.lanes.set(illuminator.id, {illuminator: clone(illuminator), ...created});
    }
  }

  get enabled() { return this.lanes.size > 0; }

  lane(id) { return this.lanes.get(String(id)); }

  get configEpoch() {
    return this.lanes.values().next().value?.operational?.configEpoch ?? null;
  }

  get receiverEpoch() {
    return this.lanes.values().next().value?.operational?.receiverEpoch ?? null;
  }

  captureOwner(record) {
    const lane = record?.illuminator ? this.lane(record.illuminator) : null;
    if (!lane || typeof record?.laneEventId !== 'string') return null;
    return {statistics: lane.operational, record: {...record, eventId: record.laneEventId}};
  }

  prepareCaptureAdmissions(records, revision = null) {
    const admissions = {};
    for (const record of records || []) {
      const owner = this.captureOwner(record);
      if (!owner) continue;
      Object.assign(admissions,
        owner.statistics.prepareCaptureAdmissions([owner.record], revision));
    }
    return admissions;
  }

  recordCaptureAdmissions(admissions, records = []) {
    for (const record of records) {
      const owner = this.captureOwner(record);
      if (!owner) continue;
      const selected = {};
      for (const window of record.captureWindows || [])
        if (admissions?.[window.captureId]) selected[window.captureId] = admissions[window.captureId];
      if (Object.keys(selected).length) owner.statistics.recordCaptureAdmissions(selected);
    }
  }

  acceptCaptureEvents(records) {
    for (const record of records || []) {
      const owner = this.captureOwner(record);
      if (owner) owner.statistics.acceptCaptureEvents([owner.record]);
    }
  }

  descriptor(id, timing = 'exact') {
    const lane = this.lane(id);
    if (!lane) return null;
    const statistics = timing === 'provisional' ? lane.operational : lane.exact;
    return identity(lane.illuminator, statistics);
  }

  accept(frame) {
    const lane = this.lane(frame?.id);
    if (!lane) return false;
    for (const statistics of [lane.exact, lane.operational]) {
      statistics.acceptDetection(frame.detections);
      statistics.acceptTracker(frame.tracks);
    }
    const eventTime = frame.detections?.eventTime || frame.tracks?.eventTime || null;
    const laneRate = Number(lane.illuminator.output_sample_rate_hz);
    const captureStride = Number.isFinite(this.captureSampleRateHz) &&
      Number.isFinite(laneRate) && laneRate > 0 ?
      Math.max(1, Math.round(this.captureSampleRateHz / laneRate)) : 1;
    const timing = {timestamp: frame.timestamp, captureSamplePositionAvailable: true,
      // New native frames carry the recorder's authoritative raw-capture
      // epoch and half-open CPI bounds. Legacy frames fall back to lane sample
      // positions; event-time epochs are not recorder epochs.
      captureEpoch: frame.capture_sample_position_available === true ?
        frame.capture_epoch : 0,
      captureFirstEpochSample: frame.capture_sample_position_available === true ?
        frame.capture_first_input_sample : frame.first_input_sample,
      captureEndEpochSample: frame.capture_sample_position_available === true ?
        frame.capture_end_input_sample : frame.end_input_sample + captureStride};
    const observed = {...timing, eventTime: eventTime ||
      {source: 'multi_illuminator_frame_timestamp', exact: false,
        estimateAvailable: false, uncertaintyBounded: false}};
    lane.exact.acceptTiming(observed);
    lane.operational.acceptTiming(provisionalTimingFrame(observed,
      'multi_illuminator_host_arrival_estimate'));
    return true;
  }

  associationPasses({timing = 'provisional', durationMs = 24 * 60 * 60 * 1000} = {}) {
    return [...this.lanes.values()].flatMap(lane => {
      const statistics = timing === 'exact' ? lane.exact : lane.operational;
      const channel = lane.illuminator.id;
      return (typeof statistics.associationPasses === 'function' ?
        statistics.associationPasses({durationMs}) : []).map(pass => ({...pass, channel}));
    });
  }

  response(kind, {illuminator, timing = 'exact', options = {}} = {}) {
    const selected = illuminator === undefined ? null : this.lane(illuminator);
    if (illuminator !== undefined && !selected) return null;
    const render = lane => {
      const statistics = timing === 'provisional' ? lane.operational : lane.exact;
      const value = kind === 'scorecard' ? statistics.liveStatus() :
        kind === 'coverage' ? statistics.radarCoverage(options) : statistics.status(options);
      return {...value, identity: identity(lane.illuminator, statistics),
        selected_channel: lane.illuminator.id, timing_policy: timing,
        association_statistics: associationStatistics(value),
        scope: 'single-illuminator'};
    };
    if (selected) return render(selected);
    const lanes = [...this.lanes.values()];
    const channels = lanes.map(render);
    const passSets = lanes.map(lane => {
      const statistics = timing === 'provisional' ? lane.operational : lane.exact;
      return typeof statistics.associationPasses === 'function' ? statistics.associationPasses() : [];
    });
    const directionalPassSets = lanes.map(lane => {
      const statistics = timing === 'provisional' ? lane.operational : lane.exact;
      return typeof statistics.associationPasses === 'function' ?
        statistics.associationPasses({durationMs: 24 * 60 * 60 * 1000}) : [];
    });
    return {schema: `vectorwarp-${kind}-aggregate-v1`, scope: 'all-illuminators-non-merged',
      aggregate: true, selected_channel: null, timing_policy: timing,
      definition: 'Channel results remain independent. The explicit overall association rollup sums channel opportunities and does not deduplicate an aircraft observed by multiple channels.',
      overall_association_statistics: aggregateAssociationStatistics(channels, passSets,
        directionalPassSets),
      illuminators: channels};
  }

  associationBasis({timing = 'exact'} = {}) {
    const channels = [...this.lanes.values()].map(lane => {
      const statistics = timing === 'provisional' ? lane.operational : lane.exact;
      const status = statistics.liveStatus();
      const population = status.track_population || {};
      return {identity: identity(lane.illuminator, statistics),
        selected_channel: lane.illuminator.id, updated_at: status.updated_at,
        association_statistics: associationStatistics(status),
        track_population: {...population,
          aircraft_like_candidates: (population.aircraft_like_candidates || []).slice(0, 10)},
        passes: typeof statistics.associationPasses === 'function' ?
          statistics.associationPasses() : []};
    });
    return {schema: 'vectorwarp-association-basis-v1', timing_policy: timing,
      channels};
  }

  close() {
    for (const lane of this.lanes.values()) {
      lane.exact.close(); lane.operational.close();
    }
  }
}

module.exports = {enabledIlluminators, laneConfig, laneAdsbSource, primaryRadarContext, statisticsFilename,
  captureDependencies, associationStatistics, aggregateAssociationStatistics,
  deduplicatedAssociationStatistics,
  aggregateTrackPopulations,
  MultiIlluminatorEvidence};
