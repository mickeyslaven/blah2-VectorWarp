'use strict';

const DEFAULT_PRE_SECONDS = 2;
const DEFAULT_POST_SECONDS = 3;
const MAX_CLIP_SECONDS = 10;
const MAX_CLIPS = 64;
const MAX_EVENT_IDS = 4096;
const MAX_ID_LENGTH = 256;
const MIN_PERIODIC_INTERVAL_SECONDS = 10;
const MAX_PERIODIC_SNAPSHOTS = 3;

const finite = value => typeof value === 'number' && Number.isFinite(value);

function optionsFromEnvironment(environment = process.env, config = {}) {
  const read = (name, fallback) => {
    if (environment[name] === undefined || environment[name] === '') return fallback;
    const value = Number(environment[name]);
    if (!finite(value) || value < 0)
      throw new Error(`${name} must be a non-negative number.`);
    return value;
  };
  const preSeconds = read('BLAH2_CAPTURE_TRIGGER_PRE_SECONDS', DEFAULT_PRE_SECONDS);
  const postSeconds = read('BLAH2_CAPTURE_TRIGGER_POST_SECONDS', DEFAULT_POST_SECONDS);
  const enabledValue = environment.BLAH2_CAPTURE_TRIGGER_ENABLED;
  if (enabledValue !== undefined && !['0', '1'].includes(enabledValue))
    throw new Error('BLAH2_CAPTURE_TRIGGER_ENABLED must be 0 or 1.');
  const enabled = enabledValue !== '0';
  const periodic = config.capture?.periodic_snapshots || {};
  const periodicIntervalSeconds = read('BLAH2_CAPTURE_TRIGGER_PERIODIC_INTERVAL_SECONDS',
    periodic.interval_seconds ?? 0);
  const periodicMaxSnapshots = read('BLAH2_CAPTURE_TRIGGER_PERIODIC_MAX_SNAPSHOTS',
    periodic.max_snapshots ?? 0);
  if (preSeconds + postSeconds > MAX_CLIP_SECONDS)
    throw new Error('Capture trigger pre/post seconds exceed the native ten-second clip limit.');
  if (periodicIntervalSeconds && periodicIntervalSeconds < MIN_PERIODIC_INTERVAL_SECONDS)
    throw new Error(`BLAH2_CAPTURE_TRIGGER_PERIODIC_INTERVAL_SECONDS must be at least ${MIN_PERIODIC_INTERVAL_SECONDS}.`);
  if (!Number.isInteger(periodicMaxSnapshots) || periodicMaxSnapshots > MAX_PERIODIC_SNAPSHOTS)
    throw new Error(`BLAH2_CAPTURE_TRIGGER_PERIODIC_MAX_SNAPSHOTS must be an integer no greater than ${MAX_PERIODIC_SNAPSHOTS}.`);
  if ((periodicIntervalSeconds === 0) !== (periodicMaxSnapshots === 0))
    throw new Error('Periodic capture interval and maximum snapshots must be enabled together.');
  return {enabled, preSeconds, postSeconds, maxClipSeconds: MAX_CLIP_SECONDS,
    periodicIntervalSeconds, periodicMaxSnapshots};
}

function validId(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= MAX_ID_LENGTH &&
    !/[\u0000-\u001f\u007f]/.test(value);
}

function compact(clip) {
  return clip ? {clipRevision: clip.revision, eventIds: [...clip.eventIds],
    captureIds: {...clip.captureIds},
    passIds: [...clip.passIds], requestedEventTime: clip.requestedEventTime,
    preSeconds: clip.preSeconds, postSeconds: clip.postSeconds,
    windowStart: clip.windowStart, windowEnd: clip.windowEnd,
    pending: clip.pending, acknowledged: clip.acknowledged,
    failure: clip.failure, file: clip.file || '', samples: clip.samples || 0} : null;
}

class CaptureTriggerQueue {
  constructor({enabled = true, preSeconds = DEFAULT_PRE_SECONDS, postSeconds = DEFAULT_POST_SECONDS,
    maxClipSeconds = MAX_CLIP_SECONDS, maxClips = MAX_CLIPS,
    maxEventIds = MAX_EVENT_IDS, periodicIntervalSeconds = 0,
    periodicMaxSnapshots = 0, now = Date.now} = {}) {
    if (!finite(preSeconds) || preSeconds < 0 || !finite(postSeconds) || postSeconds < 0)
      throw new Error('Capture trigger pre/post seconds must be non-negative.');
    if (!finite(maxClipSeconds) || maxClipSeconds <= 0 ||
        preSeconds + postSeconds > maxClipSeconds)
      throw new Error('Capture trigger windows must fit within the maximum clip duration.');
    if (!Number.isInteger(maxClips) || maxClips < 1 ||
        !Number.isInteger(maxEventIds) || maxEventIds < 1)
      throw new Error('Capture trigger bounds must be positive integers.');
    if (!finite(periodicIntervalSeconds) || periodicIntervalSeconds < 0 ||
        (periodicIntervalSeconds && periodicIntervalSeconds < MIN_PERIODIC_INTERVAL_SECONDS) ||
        !Number.isInteger(periodicMaxSnapshots) || periodicMaxSnapshots < 0 ||
        periodicMaxSnapshots > MAX_PERIODIC_SNAPSHOTS ||
        Boolean(periodicIntervalSeconds) !== Boolean(periodicMaxSnapshots))
      throw new Error('Periodic capture settings must be disabled together or use a bounded interval and count.');
    this.enabled = enabled !== false;
    this.preSeconds = preSeconds; this.postSeconds = postSeconds;
    this.maxClipSeconds = maxClipSeconds;
    this.maxClips = maxClips; this.maxEventIds = maxEventIds; this.now = now;
    this.periodicIntervalMs = periodicIntervalSeconds * 1000;
    this.periodicMaxSnapshots = periodicMaxSnapshots;
    this.revision = 0; this.clips = []; this.events = new Map();
    this.droppedEvents = 0;
  }

  remember(event) {
    const record = {eventId: event.id, passId: event.pass_id,
      requestedEventTime: event.observed_at, state: 'pending',
      clipRevisions: [], captureWindows: [], windowStart: null, windowEnd: null, failure: null};
    this.events.set(event.id, record);
    while (this.events.size > this.maxEventIds) this.events.delete(this.events.keys().next().value);
    return record;
  }

  linkRevision(clip) {
    for (const eventId of clip.eventIds) {
      const record = this.events.get(eventId);
      if (record) {
        const captureId = clip.captureIds[eventId];
        const window = record.captureWindows.find(item => item.captureId === captureId);
        if (window) Object.assign(window, {revision: clip.revision,
          requestedEventTime: clip.requestedEventTime, windowStart: clip.windowStart,
          windowEnd: clip.windowEnd});
        record.clipRevisions = [...new Set(record.captureWindows.map(item => item.revision))];
        record.windowStart = clip.windowStart;
        record.windowEnd = clip.windowEnd;
      }
    }
  }

  captureId(record) { return `capture:${record.eventId}:${record.captureWindows.length + 1}`; }

  addWindow(record, clip, kind) {
    const captureId = this.captureId(record);
    clip.captureIds[record.eventId] = captureId;
    record.captureWindows.push({captureId, kind, revision: clip.revision,
      requestedEventTime: clip.requestedEventTime, windowStart: clip.windowStart,
      windowEnd: clip.windowEnd, state: 'pending', failure: null, file: '', samples: 0,
      preRollRetries: 0});
    record.clipRevisions = [...new Set(record.captureWindows.map(item => item.revision))];
    record.windowStart = clip.windowStart; record.windowEnd = clip.windowEnd;
  }

  addDroppedWindow(record, kind, requestedEventTime, failure) {
    const captureId = this.captureId(record);
    const window = {captureId, kind, revision: null, requestedEventTime,
      windowStart: requestedEventTime - this.preSeconds * 1000,
      windowEnd: requestedEventTime + this.postSeconds * 1000,
      state: 'dropped', failure, file: '', samples: 0};
    record.captureWindows.push(window);
    record.windowStart = window.windowStart; record.windowEnd = window.windowEnd;
    return window;
  }

  terminalWindow(clip, eventId) {
    const record = this.events.get(eventId);
    const window = record?.captureWindows.find(item => item.captureId === clip.captureIds[eventId]);
    if (!window) return;
    Object.assign(window, {revision: clip.revision,
      state: clip.acknowledged ? 'acknowledged' : 'failed', failure: clip.failure,
      file: clip.file, samples: clip.samples});
  }

  acceptEligibilityEvent(event) {
    if (!this.enabled) return {accepted: false, reason: 'disabled'};
    if (!event || event.state !== 'pending' || !validId(event.id) ||
        !event.id.startsWith('eligibility:') || !validId(event.pass_id) ||
        !finite(event.observed_at) || event.observed_at < 0 ||
        !finite(event.predicted?.delay_km) || !finite(event.predicted?.doppler_hz) ||
        !validId(event.receiver_epoch) || !validId(event.config_epoch) ||
        !validId(event.site_epoch)) return {accepted: false, reason: 'invalid-event'};
    if (this.events.has(event.id)) return {accepted: false, reason: 'duplicate-event'};
    const eventRecord = this.remember(event);

    const eventStart = event.observed_at - this.preSeconds * 1000;
    const eventEnd = event.observed_at + this.postSeconds * 1000;
    const maximumMs = this.maxClipSeconds * 1000;
    const overlapping = this.clips.filter(clip => clip.pending && !clip.exposed &&
      eventStart <= clip.windowEnd && eventEnd >= clip.windowStart &&
      Math.max(eventEnd, clip.windowEnd) - Math.min(eventStart, clip.windowStart) <= maximumMs);
    let clip;
    if (overlapping.length) {
      clip = overlapping[0];
      clip.eventIds.push(event.id); clip.passIds.push(event.pass_id);
      clip.eventIds = [...new Set(clip.eventIds)]; clip.passIds = [...new Set(clip.passIds)];
      clip.requestedEventTime = Math.min(clip.requestedEventTime, event.observed_at);
      clip.windowStart = Math.min(clip.windowStart, eventStart);
      clip.windowEnd = Math.max(clip.windowEnd, eventEnd);
      clip.preSeconds = (clip.requestedEventTime - clip.windowStart) / 1000;
      clip.postSeconds = (clip.windowEnd - clip.requestedEventTime) / 1000;
      // A queued clip can be extended before the current clip is acknowledged.
      // Renumber it and every later pending clip so polling can never observe a
      // lower revision after advancing through the queue.
      const first = this.clips.indexOf(clip);
      for (let index = first; index < this.clips.length; ++index) {
        if (!this.clips[index].pending) continue;
        this.clips[index].revision = ++this.revision;
        this.linkRevision(this.clips[index]);
      }
    } else {
      while (this.clips.length >= this.maxClips) {
        const completed = this.clips.findIndex(item => !item.pending);
        if (completed < 0) break;
        this.clips.splice(completed, 1);
      }
      if (this.clips.length >= this.maxClips) {
        eventRecord.state = 'dropped'; eventRecord.failure = 'queue-capacity';
        this.droppedEvents += 1;
        return {accepted: true, coalesced: false, dropped: true,
          reason: 'queue-capacity', event: {...eventRecord}};
      }
      clip = {revision: ++this.revision, eventIds: [event.id], passIds: [event.pass_id], captureIds: {},
        requestedEventTime: event.observed_at, preSeconds: this.preSeconds,
        postSeconds: this.postSeconds, windowStart: eventStart, windowEnd: eventEnd,
        pending: true, acknowledged: false, failure: null};
      this.clips.push(clip);
    }
    this.addWindow(eventRecord, clip, 'initial');
    return {accepted: true, coalesced: overlapping.length > 0, clip: compact(clip)};
  }

  acceptSnapshot(event, requestedEventTime, kind) {
    if (!this.enabled) return {accepted: false, reason: 'disabled'};
    if (!['periodic', 'track'].includes(kind) || !event || !validId(event.id) ||
        !finite(requestedEventTime) || requestedEventTime < 0) return {accepted: false,
      reason: 'snapshot-invalid'};
    if (kind === 'periodic' && !this.periodicMaxSnapshots)
      return {accepted: false, reason: 'periodic-disabled-or-invalid'};
    const record = this.events.get(event.id);
    if (!record || record.passId !== event.pass_id) return {accepted: false, reason: 'unknown-parent'};
    const sameKind = record.captureWindows.filter(item => item.kind === kind);
    if (kind === 'periodic' && sameKind.length >= this.periodicMaxSnapshots)
      return {accepted: false, reason: 'periodic-cap'};
    if (kind === 'track' && sameKind.length)
      return {accepted: false, reason: 'track-cap'};
    const last = record.captureWindows.at(-1);
    if (kind === 'periodic' && last &&
        requestedEventTime - last.requestedEventTime < this.periodicIntervalMs)
      return {accepted: false, reason: 'periodic-interval'};
    const start = requestedEventTime - this.preSeconds * 1000;
    const end = requestedEventTime + this.postSeconds * 1000;
    const clip = this.clips.find(item => item.pending && !item.exposed && start <= item.windowEnd &&
      end >= item.windowStart && Math.max(end, item.windowEnd) - Math.min(start, item.windowStart) <=
        this.maxClipSeconds * 1000);
    if (clip) {
      clip.eventIds.push(event.id); clip.passIds.push(event.pass_id);
      clip.eventIds = [...new Set(clip.eventIds)]; clip.passIds = [...new Set(clip.passIds)];
      clip.requestedEventTime = Math.min(clip.requestedEventTime, requestedEventTime);
      clip.windowStart = Math.min(clip.windowStart, start); clip.windowEnd = Math.max(clip.windowEnd, end);
      clip.preSeconds = (clip.requestedEventTime - clip.windowStart) / 1000;
      clip.postSeconds = (clip.windowEnd - clip.requestedEventTime) / 1000;
      const first = this.clips.indexOf(clip);
      for (let index = first; index < this.clips.length; ++index) {
        if (!this.clips[index].pending) continue;
        this.clips[index].revision = ++this.revision;
        this.linkRevision(this.clips[index]);
      }
      this.addWindow(record, clip, kind);
      return {accepted: true, coalesced: true, clip: compact(clip)};
    }
    while (this.clips.length >= this.maxClips) {
      const completed = this.clips.findIndex(item => !item.pending);
      if (completed < 0) break;
      this.clips.splice(completed, 1);
    }
    if (this.clips.length >= this.maxClips) {
      const window = this.addDroppedWindow(record, kind, requestedEventTime, 'queue-capacity');
      this.droppedEvents += 1;
      return {accepted: true, dropped: true, reason: 'queue-capacity', window};
    }
    const next = {revision: ++this.revision, eventIds: [event.id], passIds: [event.pass_id], captureIds: {},
      requestedEventTime, preSeconds: this.preSeconds, postSeconds: this.postSeconds,
      windowStart: start, windowEnd: end, pending: true, acknowledged: false, failure: null};
    this.clips.push(next); this.addWindow(record, next, kind);
    return {accepted: true, coalesced: false, clip: compact(next)};
  }

  acceptPeriodicSnapshot(event, requestedEventTime = this.now()) {
    return this.acceptSnapshot(event, requestedEventTime, 'periodic');
  }

  acceptTrackSnapshot(event, requestedEventTime = this.now()) {
    return this.acceptSnapshot(event, requestedEventTime, 'track');
  }

  acknowledge(revision, failure = null, {file = '', samples = 0} = {}) {
    const clip = this.clips.find(item => item.pending && item.revision === revision);
    if (!clip) return false;
    clip.pending = false;
    clip.failure = typeof failure === 'string' && failure.trim() ?
      failure.replace(/[\u0000-\u001f\u007f]+/g, ' ').trim().slice(0, 512) : null;
    clip.acknowledged = clip.failure === null;
    clip.file = clip.acknowledged && typeof file === 'string' ? file : '';
    clip.samples = clip.acknowledged && Number.isSafeInteger(samples) && samples >= 0 ?
      samples : 0;
    for (const eventId of clip.eventIds) {
      const event = this.events.get(eventId);
      if (event) {
        event.state = clip.acknowledged ? 'acknowledged' : 'failed';
        event.failure = clip.failure;
        event.file = clip.file;
        event.samples = clip.samples;
        this.terminalWindow(clip, eventId);
      }
    }
    return true;
  }

  retryPreRollFailure(revision, requestedEventTime = this.now()) {
    const priorIndex = this.clips.findIndex(item => !item.pending && item.revision === revision);
    const prior = priorIndex < 0 ? null : this.clips[priorIndex];
    if (!prior || prior.failure !== 'Kraken clip pre-roll is unavailable' ||
        !finite(requestedEventTime) || requestedEventTime < 0) return null;
    const records = prior.eventIds.map(id => this.events.get(id)).filter(Boolean);
    const windows = records.map(event => event.captureWindows.find(item =>
      item.captureId === prior.captureIds[event.eventId] && item.revision === revision));
    // Retry each physical capture window once. Events can legitimately have an
    // earlier acknowledged initial window plus a later periodic window, so the
    // event-wide revision count cannot be used as the retry guard.
    if (!records.length || windows.some(window => !window || window.preRollRetries >= 1)) return null;
    const clip = {revision: ++this.revision, eventIds: [...prior.eventIds], captureIds: {...prior.captureIds},
      passIds: [...prior.passIds], requestedEventTime,
      preSeconds: this.preSeconds, postSeconds: this.postSeconds,
      windowStart: requestedEventTime - this.preSeconds * 1000,
      windowEnd: requestedEventTime + this.postSeconds * 1000,
      pending: true, acknowledged: false, failure: null};
    // Append retries after every already-issued request. Once the recorder has
    // observed a revision its identity is immutable; renumbering it would make
    // the eventual terminal acknowledgement impossible to match.
    this.clips.splice(priorIndex, 1);
    this.clips.push(clip);
    for (let index = 0; index < records.length; ++index) {
      const event = records[index];
      event.state = 'pending'; event.failure = null;
      const window = windows[index];
      if (window) Object.assign(window, {revision: clip.revision, requestedEventTime,
        windowStart: clip.windowStart, windowEnd: clip.windowEnd, state: 'pending', failure: null,
        preRollRetries: window.preRollRetries + 1});
      event.clipRevisions = [...new Set([...event.clipRevisions, clip.revision])];
      event.windowStart = clip.windowStart;
      event.windowEnd = clip.windowEnd;
    }
    return compact(clip);
  }

  eventsForRevision(revision) {
    return [...this.events.values()].filter(event =>
      event.clipRevisions.includes(revision)).map(event =>
      ({...event, clipRevisions: [...event.clipRevisions]}));
  }

  setAdmission(revision, admissions, integrity) {
    let changed = false;
    for (const event of this.events.values()) for (const window of event.captureWindows || []) {
      if (window.revision !== revision || !admissions?.[window.captureId]) continue;
      const truth = admissions[window.captureId];
      window.admissionState = truth.valid && integrity?.valid ? 'admitted' : 'quarantined';
      window.admissionReasons = [...new Set([...(truth.reasons || []), ...(integrity?.reasons || [])])];
      window.truthSamples = truth.truth_samples;
      window.cpiTruth = truth.cpis;
      window.integrity = integrity;
      changed = true;
    }
    return changed;
  }

  current() {
    return this.clips.find(clip => clip.pending) || this.clips.at(-1) || null;
  }

  request(expose = false) {
    const clip = this.current();
    if (expose && clip?.pending) clip.exposed = true;
    return compact(clip) || {clipRevision: 0, eventIds: [], passIds: [],
      requestedEventTime: null, preSeconds: this.preSeconds, postSeconds: this.postSeconds,
      windowStart: null, windowEnd: null, pending: false, acknowledged: false,
      failure: null, file: '', samples: 0};
  }

  status() {
    return {...this.request(false), enabled: this.enabled,
      queuedClips: this.clips.filter(clip => clip.pending).length,
      maximumClipSeconds: this.maxClipSeconds, droppedEvents: this.droppedEvents,
      recentEvents: [...this.events.values()].slice(-250).map(event =>
        ({...event, clipRevisions: [...event.clipRevisions]}))};
  }
}

function createCaptureTriggerQueue(options) { return new CaptureTriggerQueue(options); }

module.exports = {DEFAULT_PRE_SECONDS, DEFAULT_POST_SECONDS, MAX_CLIP_SECONDS,
  MAX_CLIPS, MAX_EVENT_IDS, MIN_PERIODIC_INTERVAL_SECONDS, MAX_PERIODIC_SNAPSHOTS,
  optionsFromEnvironment, CaptureTriggerQueue,
  createCaptureTriggerQueue};
