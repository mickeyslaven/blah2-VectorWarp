'use strict';

const assert = require('assert');
const {installRecordingLibraryRoutes} = require('./recording-library-routes');

const routes = new Map();
const app = {get: (route, handler) => routes.set(`GET ${route}`, handler),
  post: (route, handler) => routes.set(`POST ${route}`, handler),
  delete: (route, handler) => routes.set(`DELETE ${route}`, handler)};
let processor = {recording: true, recordingFile: '/recordings/active.mchq'};
let listOptions;
installRecordingLibraryRoutes(app, {
  recordingLibrary: {list: async options => { listOptions = options; return {recordings: [
    {path: '/recordings/active.mchq', filename: 'active.mchq', aircraft: []}]}; },
  directories: async () => ({}), resolve: () => null, createDirectory: async () => ({}),
  remove: async () => ({}), prepareForRecording: async () => ({}), directory: () => '/recordings'},
  recordingTimeline: {load: () => null, loadReplayProfile: () => null},
  recordingPasses: () => [{pass_id: 'pass-1'}], activeRecordingFile: value => value?.recordingFile || '',
  currentProcessor: () => processor, configWriteOriginAllowed: () => false
});

assert.equal(routes.size, 10, 'all recording-library endpoints are installed together');
const response = () => ({statusCode: 200, body: null, status(code) { this.statusCode = code; return this; },
  json(value) { this.body = value; return this; }});

(async () => {
  const list = response();
  await routes.get('GET /api/recordings')({query: {offset: '2', limit: '5'}}, list, assert.fail);
  assert.equal(list.statusCode, 200);
  assert.equal(listOptions.activeFile, '/recordings/active.mchq', 'reads live processor state');
  assert.deepEqual(listOptions.passes, [{pass_id: 'pass-1'}]);
  assert.equal(listOptions.offset, 2); assert.equal(listOptions.limit, 5);
  assert.equal(list.body.recordings[0].replay_file, '/recordings/active.mchq');
  assert.equal(list.body.recordings[0].path, undefined);

  processor = null;
  const refreshed = response();
  await routes.get('GET /api/recordings')({query: {}}, refreshed, assert.fail);
  assert.equal(listOptions.activeFile, '', 'does not retain stale processor state');

  const denied = response();
  await routes.get('POST /api/recordings/import')({query: {filename: 'clip.mchq'},
    get: () => 'recording-import-v1'}, denied, assert.fail);
  assert.equal(denied.statusCode, 403);
  assert.match(denied.body.error, /trusted VectorWarp origin/);
  console.log('recording-library routes passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
