'use strict';

const assert = require('assert/strict');
const {automaticCaptureReadiness} = require('./automatic-capture-readiness');

const live = {input: 'live', state: 'live', error: ''};

for (const [name, value] of Object.entries({
  'missing processor telemetry': {processor: null, radarFrameFresh: true},
  'replay processor': {processor: {...live, input: 'replay'}, radarFrameFresh: true},
  'starting processor': {processor: {...live, state: 'starting'}, radarFrameFresh: true},
  'failed processor': {processor: {...live, error: 'receiver failed'}, radarFrameFresh: true},
  'missing live frame': {processor: live, radarFrameFresh: false}
})) assert.deepEqual(automaticCaptureReadiness(value),
  {ready: false, reason: 'radar-recorder-warming'}, name);

assert.deepEqual(automaticCaptureReadiness({processor: live, radarFrameFresh: true}),
  {ready: true, reason: null});
