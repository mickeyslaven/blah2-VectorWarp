'use strict';

const path = require('path');
const {createEvidenceStatisticsLanes, provisionalTimingFrame} =
  require('./evidence-statistics');
const {DelayDopplerHistory} = require('./adsb-geometry');

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
  return value;
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

class MultiIlluminatorEvidence {
  constructor(config, adsbSource, {baseFilename = process.env.BLAH2_STATISTICS_PATH ||
    '/var/lib/vectorwarp-api/evidence-statistics.json', createLanes = createEvidenceStatisticsLanes,
    dependencies = {}} = {}) {
    this.lanes = new Map();
    for (const illuminator of enabledIlluminators(config)) {
      const lane = laneConfig(config, illuminator);
      const created = createLanes(lane, laneAdsbSource(lane, adsbSource), {
        exact: {...dependencies,
          filename: statisticsFilename(baseFilename, illuminator.id, 'exact')},
        operational: {...dependencies,
          filename: statisticsFilename(baseFilename, illuminator.id, 'provisional'),
          onEligibilityEvent: () => {}, onPeriodicCapture: () => {}, onTrackCapture: () => {}}});
      this.lanes.set(illuminator.id, {illuminator: clone(illuminator), ...created});
    }
  }

  get enabled() { return this.lanes.size > 0; }

  lane(id) { return this.lanes.get(String(id)); }

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
    const timing = {timestamp: frame.timestamp, captureSamplePositionAvailable: true,
      captureEpoch: 0, captureFirstEpochSample: frame.first_input_sample,
      captureEndEpochSample: frame.end_input_sample + 1};
    const observed = {...timing, eventTime: eventTime ||
      {source: 'multi_illuminator_frame_timestamp', exact: false,
        estimateAvailable: false, uncertaintyBounded: false}};
    lane.exact.acceptTiming(observed);
    lane.operational.acceptTiming(provisionalTimingFrame(observed,
      'multi_illuminator_host_arrival_estimate'));
    return true;
  }

  response(kind, {illuminator, timing = 'exact', options = {}} = {}) {
    const selected = illuminator === undefined ? null : this.lane(illuminator);
    if (illuminator !== undefined && !selected) return null;
    const render = lane => {
      const statistics = timing === 'provisional' ? lane.operational : lane.exact;
      const value = kind === 'scorecard' ? statistics.liveStatus() :
        kind === 'coverage' ? statistics.radarCoverage(options) : statistics.status(options);
      return {...value, identity: identity(lane.illuminator, statistics),
        scope: 'single-illuminator'};
    };
    if (selected) return render(selected);
    return {schema: `vectorwarp-${kind}-aggregate-v1`, scope: 'all-illuminators-non-merged',
      aggregate: true,
      definition: 'Lane results are listed independently; counts, coverage and configurations are not merged.',
      illuminators: [...this.lanes.values()].map(render)};
  }

  close() {
    for (const lane of this.lanes.values()) {
      lane.exact.close(); lane.operational.close();
    }
  }
}

module.exports = {enabledIlluminators, laneConfig, laneAdsbSource, statisticsFilename,
  MultiIlluminatorEvidence};
