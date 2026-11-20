'use strict';

const crypto = require('crypto');
const {distanceM} = require('./cooperative-detection-tracker');
const {MultiIlluminatorTrackFuser} = require('./multi-illuminator-track-fusion');

const C_KM_S = 299792.458;
const finite = value => typeof value !== 'boolean' && value !== null && value !== '' && value !== undefined &&
  Number.isFinite(Number(value));
const digest = value => crypto.createHash('sha256').update(JSON.stringify(value)).digest('hex');
const validLocation = value => [value?.latitude, value?.longitude, value?.altitude].every(finite) &&
  Math.abs(Number(value.latitude)) <= 90 && Math.abs(Number(value.longitude)) <= 180;
const domainKey = item => `${item.server_session_id}:${item.config_revision}:${item.lineage.epoch}`;

class FederatedGeographicFusion {
  constructor(config, {coLocationRadiusM = 5, maximumSkewMs = 1250} = {}) {
    this.baseConfig = config; this.coLocationRadiusM = coLocationRadiusM;
    this.maximumSkewMs = maximumSkewMs; this.fuser = null; this.fingerprint = null;
    this.lastTokens = new Map(); this.lastResult = null; this.lastError = null;
    this.sourceWatermarks = new Map();
    this.retiredDomains = new Map(); this.sourceCatalog = new Map();
    this.lastUpdateAt = null; this.sourceStatus = {accepted: [], rejected: []};
  }
  reset() {
    this.fuser = null; this.fingerprint = null; this.lastTokens.clear();
    this.lastResult = null; this.lastError = null; this.lastUpdateAt = null;
    this.sourceWatermarks.clear();
    this.retiredDomains.clear(); this.sourceCatalog.clear();
  }
  update(bundles, now = Date.now()) {
    const receiver = this.baseConfig?.location?.rx, accepted = [], rejected = [];
    if (!validLocation(receiver)) return this.unavailable('Coordinator receiver geometry is invalid.');
    for (const bundle of bundles || []) {
      const transport = bundle?.federation_transport || {};
      const requestStartedAt = Number(transport.request_started_at);
      const receivedAt = Number(transport.received_at);
      const generatedAt = Number(bundle?.generated_at);
      if (bundle?.schema !== 'vectorwarp-radar-observation-bundle-v1' ||
          !bundle.node_id || !bundle.server_session_id || !bundle.config_revision ||
          !Array.isArray(bundle.channels) || !finite(generatedAt) ||
          !finite(requestStartedAt) || !finite(receivedAt) || receivedAt < requestStartedAt ||
          receivedAt - requestStartedAt > 3000) { rejected.push({node: bundle?.node_id || null,
            reason: 'invalid-observation-bundle'}); continue; }
      const receiptMidpoint = (requestStartedAt + receivedAt) / 2;
      const receiptUncertaintyMs = Math.max(1, (receivedAt - requestStartedAt) / 2);
      for (const channel of bundle.channels) {
        const key = String(channel?.source_key || '');
        if (channel?.node_id !== bundle.node_id || !key.startsWith(`${bundle.node_id}:`) ||
            !validLocation(channel.receiver) || !validLocation(channel.transmitter) ||
            !finite(channel.frequency_hz) || Number(channel.frequency_hz) <= 0 ||
            !finite(channel.delay_reference_sample_rate_hz) ||
            Number(channel.delay_reference_sample_rate_hz) <= 0 ||
            !finite(channel.cpi_seconds) || Number(channel.cpi_seconds) <= 0 ||
            Number(channel.cpi_seconds) > 60 ||
            !['producer-bounded-event-time', 'producer-unbounded-event-time'].includes(
              channel.measurement_time?.basis) ||
            channel.measurement_time?.basis === 'producer-bounded-event-time' &&
              (!finite(channel.measurement_time?.uncertainty_ms) ||
               Number(channel.measurement_time.uncertainty_ms) < 0 ||
               Number(channel.measurement_time.uncertainty_ms) > Number(channel.cpi_seconds) * 1000) ||
            channel.altitude_datum !== 'WGS84-ellipsoid' ||
            channel.delay_units !== 'bins-at-delay-reference-sample-rate' ||
            channel.doppler_units !== 'hz-positive-closing' ||
            !finite(channel.timestamp) || Number(channel.tracks?.timestamp) !== Number(channel.timestamp) ||
            !Array.isArray(channel.tracks?.data) || channel.tracks.data.some(track =>
              !track || track.id === null || track.id === undefined ||
              !['ACTIVE', 'CONFIRMED', 'COASTING', 'ASSOCIATED'].includes(
                String(track.state || '').toUpperCase()) ||
              !finite(track.delay) || !finite(track.doppler) ||
              track.delay_sigma_bins !== undefined &&
                (!finite(track.delay_sigma_bins) || Number(track.delay_sigma_bins) < 0))) {
          rejected.push({source: key || null, reason: 'invalid-channel-observation'}); continue;
        }
        const receiverSeparationM = distanceM(receiver, channel.receiver);
        if (receiverSeparationM > this.coLocationRadiusM) {
          rejected.push({source: key, reason: 'distinct-receiver-geographic-solver-not-qualified'}); continue;
        }
        const lineage = channel.lineage || {};
        if (lineage.epoch === undefined || lineage.epoch === null ||
            lineage.first_sample === undefined || lineage.first_sample === null ||
            lineage.end_sample === undefined || lineage.end_sample === null ||
            !/^\d+$/.test(String(lineage.first_sample)) ||
            !/^\d+$/.test(String(lineage.end_sample)) ||
            BigInt(lineage.end_sample) <= BigInt(lineage.first_sample)) {
          rejected.push({source: key, reason: 'missing-source-lineage'}); continue;
        }
        const sourceAgeMs = generatedAt - Number(channel.timestamp);
        const maximumSourceAgeMs = Math.max(10000, 8 * Number(channel.cpi_seconds) * 1000);
        const effectiveTimestamp = receiptMidpoint - sourceAgeMs;
        if (sourceAgeMs < -250 || sourceAgeMs > maximumSourceAgeMs ||
            effectiveTimestamp > now + this.maximumSkewMs || now - effectiveTimestamp > maximumSourceAgeMs) {
          rejected.push({source: key, reason: 'stale-or-future-source-observation'}); continue;
        }
        accepted.push({...channel, server_session_id: bundle.server_session_id,
          config_revision: bundle.config_revision, federation_local: bundle.federation_local === true,
          effective_timestamp: effectiveTimestamp, receipt_uncertainty_ms: receiptUncertaintyMs,
          timing_bounded: channel.measurement_time.basis === 'producer-bounded-event-time',
          measurement_uncertainty_ms: channel.measurement_time.basis === 'producer-bounded-event-time' ?
            Number(channel.measurement_time.uncertainty_ms) : Number(channel.cpi_seconds) * 500,
          receiver_separation_m: receiverSeparationM});
      }
    }
    accepted.sort((a, b) => a.source_key.localeCompare(b.source_key));
    const eligible = accepted.filter(item => {
      if ((this.retiredDomains.get(item.source_key) || new Set()).has(domainKey(item))) {
        rejected.push({source: item.source_key, reason: 'retired-source-domain'}); return false;
      }
      return true;
    });
    this.sourceStatus = {accepted: eligible.map(item => item.source_key), rejected};
    if (eligible.length < 3) return this.unavailable('At least three co-located-receiver radar sources are required.');
    const targetItem = eligible.reduce((latest, item) =>
      Number(item.effective_timestamp) > Number(latest.effective_timestamp) ? item : latest);
    const alignmentTargetTimestamp = Number(targetItem.effective_timestamp);
    const alignmentTargetUncertaintyMs = Number(targetItem.receipt_uncertainty_ms) +
      Number(targetItem.measurement_uncertainty_ms);
    let aligned = eligible.filter(item => alignmentTargetTimestamp - Number(item.effective_timestamp) +
      alignmentTargetUncertaintyMs + Number(item.receipt_uncertainty_ms) +
      Number(item.measurement_uncertainty_ms) <= this.maximumSkewMs);
    for (const item of eligible) if (!aligned.includes(item))
      rejected.push({source: item.source_key, reason: 'outside-bounded-alignment-window'});
    if (aligned.length < 2)
      return this.unavailable('Fewer than two radar sources are inside the bounded alignment window.');
    const tokens = eligible.map(item => `${item.source_key}:${item.server_session_id}:` +
      `${item.config_revision}:${item.lineage.epoch}:${item.lineage.first_sample}:` +
      `${item.lineage.end_sample}`);
    const tokenBySource = new Map(eligible.map((item, index) => [item.source_key, tokens[index]]));
    let advancing = aligned.filter(item => this.lastTokens.get(item.source_key) !==
      tokenBySource.get(item.source_key));
    advancing = advancing.filter(item => {
      const lineage = item.lineage, prior = this.sourceWatermarks.get(item.source_key);
      const sameSession = prior && prior.session === item.server_session_id &&
        prior.revision === item.config_revision && prior.epoch === String(lineage.epoch);
      if (sameSession && BigInt(lineage.first_sample) < BigInt(prior.end_sample)) {
        rejected.push({source: item.source_key, reason: 'overlapping-or-replayed-source-interval'}); return false;
      }
      return true;
    });
    if (advancing.length < 2)
      return rejected.some(item => item.reason === 'overlapping-or-replayed-source-interval') ?
        this.unavailable('New observations overlap or replay already consumed source intervals.') : this.snapshot(now);
    let consumedTarget, targetTimestamp, targetTimingUncertaintyMs;
    for (;;) {
      consumedTarget = advancing.reduce((latest, item) =>
        Number(item.effective_timestamp) > Number(latest.effective_timestamp) ? item : latest);
      targetTimestamp = Number(consumedTarget.effective_timestamp);
      targetTimingUncertaintyMs = Number(consumedTarget.receipt_uncertainty_ms) +
        Number(consumedTarget.measurement_uncertainty_ms);
      const requalified = advancing.filter(item =>
        targetTimestamp - Number(item.effective_timestamp) + targetTimingUncertaintyMs +
        Number(item.receipt_uncertainty_ms) + Number(item.measurement_uncertainty_ms) <=
          this.maximumSkewMs);
      if (requalified.length === advancing.length) break;
      advancing = requalified;
      if (advancing.length < 2) return this.snapshot(now);
    }
    if (this.lastResult && finite(this.lastResult.timestamp) &&
        targetTimestamp <= Number(this.lastResult.timestamp))
      return this.unavailable('New source intervals have non-advancing observation time.');
    // Only evidence that survived alignment, replay and overlap checks may
    // change persistent source-domain state. Diagnostic or rejected arrivals
    // must not reset healthy temporal support.
    for (const item of advancing) this.sourceCatalog.set(item.source_key, item);
    const catalog = [...this.sourceCatalog.values()].sort((a, b) =>
      a.source_key.localeCompare(b.source_key));
    const sourceFingerprint = digest(catalog.map(item => ({source: item.source_key,
      receiver: item.receiver, transmitter: item.transmitter, frequency_hz: item.frequency_hz,
      sample_rate_hz: item.delay_reference_sample_rate_hz})));
    if (sourceFingerprint !== this.fingerprint) {
      const fusionConfig = JSON.parse(JSON.stringify(this.baseConfig));
      fusionConfig.location = {...fusionConfig.location, rx: receiver};
      fusionConfig.process = fusionConfig.process || {};
      fusionConfig.process.multi_illuminator = {
        ...(fusionConfig.process.multi_illuminator || {}), enabled: true,
        illuminators: catalog.map(item => ({id: item.source_key, enabled: true,
          frequency_hz: Number(item.frequency_hz),
          output_sample_rate_hz: Number(item.delay_reference_sample_rate_hz),
          transmitter: item.transmitter}))};
      this.fuser = new MultiIlluminatorTrackFuser(fusionConfig);
      this.fingerprint = sourceFingerprint; this.lastTokens.clear();
    }
    const cpiSeconds = Math.max(.05, Number(this.baseConfig?.process?.data?.cpi) || .5);
    const maximumAccelerationHzS = Math.max(0,
      Number(this.baseConfig?.process?.tracker?.initiate?.maxAcc) || 10);
    const lineageEpoch = digest(catalog.map(item => [item.source_key,
      item.server_session_id, item.config_revision, item.lineage.epoch]));
    const cohortKey = digest(advancing.map(item => tokenBySource.get(item.source_key)));
    const cohortAlignmentQualified = consumedTarget.timing_bounded === true &&
      advancing.every(item => item.timing_bounded === true);
    const frames = advancing.map(item => {
      const carrier = Number(item.frequency_hz), sampleRate = Number(item.delay_reference_sample_rate_hz);
      const resolutionKm = C_KM_S / sampleRate;
      const dt = (targetTimestamp - Number(item.effective_timestamp)) / 1000;
      const alignmentUncertaintyMs = targetTimingUncertaintyMs +
        Number(item.receipt_uncertainty_ms) + Number(item.measurement_uncertainty_ms);
      const tracks = item.tracks.data.map(track => {
        const doppler = Number(track.doppler), rangeRate = -doppler * C_KM_S / carrier;
        const propagatedKm = Number(track.delay) * resolutionKm + rangeRate * dt;
        const sourceCpiSeconds = Number(item.cpi_seconds);
        const rangeRateResolution = C_KM_S / carrier / sourceCpiSeconds;
        const acceleration = C_KM_S / carrier * maximumAccelerationHzS;
        return {...track, delay: propagatedKm / resolutionKm,
          federated_propagation_sigma_km: Math.hypot(rangeRateResolution * dt,
            .5 * acceleration * dt * dt,
            Math.abs(rangeRate) * alignmentUncertaintyMs / 1000,
            2 * Number(item.receiver_separation_m) / 1000),
          provenance: {...(track.provenance || {}), federated_alignment: {
            source_timestamp: Number(item.effective_timestamp), target_timestamp: targetTimestamp,
            propagated_seconds: dt, method: 'doppler-range-rate'}}};
      });
      return {id: item.source_key, frequency_hz: carrier, sample_rate_hz: sampleRate,
        timestamp: targetTimestamp, tracks: {timestamp: targetTimestamp, data: tracks},
        federated_lineage: {schema: 'vectorwarp-federated-radar-lineage-v1',
          cohort_key: cohortKey, lineage_epoch: lineageEpoch, target_timestamp: targetTimestamp,
          maximum_skew_ms: this.maximumSkewMs,
          evidence_token: tokenBySource.get(item.source_key),
          source_timestamp: Number(item.effective_timestamp),
          alignment_qualified: cohortAlignmentQualified,
          alignment_uncertainty_ms: cohortAlignmentQualified ? alignmentUncertaintyMs : null,
          nominal_alignment_uncertainty_ms: alignmentUncertaintyMs,
          alignment_basis: cohortAlignmentQualified ?
            'receipt-relative-source-age-with-bounded-rtt-and-doppler-propagation' :
            'diagnostic-receipt-relative-unbounded-producer-time'}};
    });
    this.lastResult = this.fuser.update(frames);
    const contributingSources = new Set([
      ...(this.lastResult?.hypotheses || []), ...(this.lastResult?.tracks || []),
      ...(this.lastResult?.position_candidates || [])
    ].flatMap(item => [...(item?.contributors || []),
      ...(item?.corroborating_contributors || [])]).map(item => item.illuminator));
    this.lastResult = {...this.lastResult, federation: {coordinator_receiver: receiver,
      source_count: eligible.length, sources: eligible.map(item => item.source_key),
      maximum_source_skew_ms: targetTimestamp - Math.min(...advancing.map(item => Number(item.effective_timestamp))),
      external_source_contributing: advancing.some(item =>
        item.federation_local !== true && contributingSources.has(item.source_key)),
      rejected_sources: rejected, adsb_used_by_solver: false}};
    for (const item of advancing) {
      const prior = this.sourceWatermarks.get(item.source_key);
      const priorDomain = prior && `${prior.session}:${prior.revision}:${prior.epoch}`;
      if (priorDomain && priorDomain !== domainKey(item)) {
        if (!this.retiredDomains.has(item.source_key)) this.retiredDomains.set(item.source_key, new Set());
        const retired = this.retiredDomains.get(item.source_key);
        retired.add(priorDomain);
        while (retired.size > 32) retired.delete(retired.values().next().value);
      }
      this.lastTokens.set(item.source_key, tokenBySource.get(item.source_key));
      this.sourceWatermarks.set(item.source_key, {session: item.server_session_id,
        revision: item.config_revision, epoch: String(item.lineage.epoch),
        end_sample: String(item.lineage.end_sample)});
    }
    this.lastUpdateAt = now; this.lastError = null;
    return this.lastResult;
  }
  unavailable(reason) {
    this.lastError = reason;
    this.lastResult = {...(this.lastResult || {}), schema: 'vectorwarp-multi-illuminator-track-fusion-v1',
      available: false, operational_localization: false, reason, hypotheses: [], tracks: [],
      position_candidates: [], federation: {...this.sourceStatus, adsb_used_by_solver: false}};
    return this.lastResult;
  }
  snapshot(now = Date.now()) {
    if (this.lastError) return this.unavailable(this.lastError);
    if (!this.fuser || !this.lastResult) return this.unavailable(this.lastError ||
      'Waiting for aligned federated radar observations.');
    const staleAfterSeconds = Math.max(.1, 2 *
      (Number(this.baseConfig?.process?.data?.cpi) || .5));
    const inputAgeSeconds = this.lastUpdateAt === null ? Infinity :
      Math.max(0, (now - this.lastUpdateAt) / 1000);
    if (inputAgeSeconds > staleAfterSeconds) return {...this.lastResult,
      available: false, operational_localization: false,
      reason: 'Federated radar observations are stale; local fusion remains authoritative.',
      tracks: [], hypotheses: [], position_candidates: [],
      federation: {...this.lastResult.federation, input_age_seconds: inputAgeSeconds}};
    const value = this.fuser.snapshot(now) || this.lastResult;
    return {...value, federation: {...this.lastResult.federation,
      input_age_seconds: inputAgeSeconds}};
  }
}

module.exports = {FederatedGeographicFusion};
