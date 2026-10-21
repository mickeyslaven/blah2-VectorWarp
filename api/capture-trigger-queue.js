'use strict';

const DEFAULT_PRE_SECONDS = 2;
const DEFAULT_POST_SECONDS = 3;
const MAX_CLIP_SECONDS = 10;
const MAX_CLIPS = 64;
const MAX_EVENT_IDS = 4096;
const MAX_ID_LENGTH = 256;

const finite = value => typeof value === 'number' && Number.isFinite(value);

function optionsFromEnvironment(environment = process.env) {
  const read = (name, fallback) => {
    if (environment[name] === undefined || environment[name] === '') return fallback;
    const value = Number(environment[name]);
    if (!finite(value) || value < 0)
      throw new Error(`${name} must be a non-negative number.`);
    return value;
  };
  const preSeconds = read('BLAH2_CAPTURE_TRIGGER_PRE_SECONDS', DEFAULT_PRE_SECONDS);
  const postSeconds = read('BLAH2_CAPTURE_TRIGGER_POST_SECONDS', DEFAULT_POST_SECONDS);
  if (preSeconds + postSeconds > MAX_CLIP_SECONDS)
    throw new Error('Capture trigger pre/post seconds exceed the native ten-second clip limit.');
  return {preSeconds, postSeconds, maxClipSeconds: MAX_CLIP_SECONDS};
}

function validId(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= MAX_ID_LENGTH &&
    !/[\u0000-\u001f\u007f]/.test(value);
}

function compact(clip) {
  return clip ? {clipRevision: clip.revision, eventIds: [...clip.eventIds],
    passIds: [...clip.passIds], requestedEventTime: clip.requestedEventTime,
    preSeconds: clip.preSeconds, postSeconds: clip.postSeconds,
    windowStart: clip.windowStart, windowEnd: clip.windowEnd,
    pending: clip.pending, acknowledged: clip.acknowledged,
    failure: clip.failure, file: clip.file || '', samples: clip.samples || 0} : null;
}

class CaptureTriggerQueue {
  constructor({preSeconds = DEFAULT_PRE_SECONDS, postSeconds = DEFAULT_POST_SECONDS,
    maxClipSeconds = MAX_CLIP_SECONDS, maxClips = MAX_CLIPS,
    maxEventIds = MAX_EVENT_IDS, now = Date.now} = {}) {
    if (!finite(preSeconds) || preSeconds < 0 || !finite(postSeconds) || postSeconds < 0)
      throw new Error('Capture trigger pre/post seconds must be non-negative.');
    if (!finite(maxClipSeconds) || maxClipSeconds <= 0 ||
        preSeconds + postSeconds > maxClipSeconds)
      throw new Error('Capture trigger windows must fit within the maximum clip duration.');
    if (!Number.isInteger(maxClips) || maxClips < 1 ||
        !Number.isInteger(maxEventIds) || maxEventIds < 1)
      throw new Error('Capture trigger bounds must be positive integers.');
    this.preSeconds = preSeconds; this.postSeconds = postSeconds;
    this.maxClipSeconds = maxClipSeconds;
    this.maxClips = maxClips; this.maxEventIds = maxEventIds; this.now = now;
    this.revision = 0; this.clips = []; this.events = new Map();
    this.droppedEvents = 0;
  }

  remember(event) {
    const record = {eventId: event.id, passId: event.pass_id,
      requestedEventTime: event.observed_at, state: 'pending',
      clipRevisions: [], failure: null};
    this.events.set(event.id, record);
    while (this.events.size > this.maxEventIds) this.events.delete(this.events.keys().next().value);
    return record;
  }

  linkRevision(clip) {
    for (const eventId of clip.eventIds) {
      const record = this.events.get(eventId);
      if (record) record.clipRevisions = [clip.revision];
    }
  }

  acceptEligibilityEvent(event) {
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
    const overlapping = this.clips.filter(clip => clip.pending &&
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
      clip = {revision: ++this.revision, eventIds: [event.id], passIds: [event.pass_id],
        requestedEventTime: event.observed_at, preSeconds: this.preSeconds,
        postSeconds: this.postSeconds, windowStart: eventStart, windowEnd: eventEnd,
        pending: true, acknowledged: false, failure: null};
      this.clips.push(clip);
    }
    eventRecord.clipRevisions = [clip.revision];
    return {accepted: true, coalesced: overlapping.length > 0, clip: compact(clip)};
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
    if (!records.length || records.some(event => event.clipRevisions.length !== 1)) return null;
    const clip = {revision: ++this.revision, eventIds: [...prior.eventIds],
      passIds: [...prior.passIds], requestedEventTime,
      preSeconds: this.preSeconds, postSeconds: this.postSeconds,
      windowStart: requestedEventTime - this.preSeconds * 1000,
      windowEnd: requestedEventTime + this.postSeconds * 1000,
      pending: true, acknowledged: false, failure: null};
    this.clips[priorIndex] = clip;
    for (const event of records) {
      event.state = 'pending'; event.failure = null;
      event.clipRevisions.push(clip.revision);
    }
    return compact(clip);
  }

  eventsForRevision(revision) {
    return [...this.events.values()].filter(event =>
      event.clipRevisions.includes(revision)).map(event =>
      ({...event, clipRevisions: [...event.clipRevisions]}));
  }

  current() {
    return this.clips.find(clip => clip.pending) || this.clips.at(-1) || null;
  }

  request() {
    return compact(this.current()) || {clipRevision: 0, eventIds: [], passIds: [],
      requestedEventTime: null, preSeconds: this.preSeconds, postSeconds: this.postSeconds,
      windowStart: null, windowEnd: null, pending: false, acknowledged: false,
      failure: null, file: '', samples: 0};
  }

  status() {
    return {...this.request(), queuedClips: this.clips.filter(clip => clip.pending).length,
      maximumClipSeconds: this.maxClipSeconds, droppedEvents: this.droppedEvents,
      recentEvents: [...this.events.values()].slice(-250).map(event =>
        ({...event, clipRevisions: [...event.clipRevisions]}))};
  }
}

function createCaptureTriggerQueue(options) { return new CaptureTriggerQueue(options); }

module.exports = {DEFAULT_PRE_SECONDS, DEFAULT_POST_SECONDS, MAX_CLIP_SECONDS,
  MAX_CLIPS, MAX_EVENT_IDS, optionsFromEnvironment, CaptureTriggerQueue,
  createCaptureTriggerQueue};
