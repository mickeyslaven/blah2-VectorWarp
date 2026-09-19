'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const {LOCAL_SOURCES, localFileSources, selectLocalAdsb, discoverLocalAdsb} = require('./adsb-discovery');
const {readJson, createAdsbSource} = require('./adsb-source');

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-mac-adsb-'));
  let clock = Date.now();
  const fresh = () => ({now: clock / 1000, aircraft: [{hex: 'abc123', lat: 51.48,
    lon: -.45, alt_baro: 10000, gs: 240, track: 90, seen_pos: 0}]});
  let payload = fresh();
  const server = http.createServer((req, res) => {
    assert.equal(req.url, '/data/aircraft.json');
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify(payload));
  });
  try {
    assert.deepEqual(localFileSources({platform: 'linux'}), LOCAL_SOURCES,
      'Linux discovery paths must remain unchanged');
    const arm = localFileSources({platform: 'darwin', arch: 'arm64', home: directory, brewPrefix: ''});
    const intel = localFileSources({platform: 'darwin', arch: 'x64', home: directory, brewPrefix: ''});
    assert.equal(arm[0].path, '/opt/homebrew/var/run/readsb/aircraft.json');
    assert.equal(intel[0].path, '/usr/local/var/run/readsb/aircraft.json');
    assert.throws(() => localFileSources({platform: 'darwin', home: directory, brewPrefix: 'relative'}), /absolute/);
    const files = localFileSources({platform: 'darwin', home: directory,
      brewPrefix: path.join(directory, 'brew with spaces')});
    assert.equal(files.length, 8);
    assert.ok(files.every(file => file.path.startsWith(directory + path.sep)));
    const homeReadsb = files.find(file => file.value === 'local:readsb' && file.path.includes('Application Support'));
    fs.mkdirSync(path.dirname(homeReadsb.path), {recursive: true});
    fs.writeFileSync(homeReadsb.path, JSON.stringify(payload));
    let selection = await selectLocalAdsb('local:readsb', {fileSources: files, now: () => clock});
    assert.equal(selection.source.path, homeReadsb.path);
    assert.equal((await selection.read()).aircraft[0].hex, 'abc123');
    selection = await discoverLocalAdsb({fileSources: files, httpSources: [], now: () => clock});
    assert.equal(selection.source.path, homeReadsb.path);
    const brewReadsb = files[0];
    fs.mkdirSync(path.dirname(brewReadsb.path), {recursive: true});
    fs.writeFileSync(brewReadsb.path, JSON.stringify(payload));
    await assert.rejects(selectLocalAdsb('local:readsb', {fileSources: files, now: () => clock}), /Multiple/);
    fs.unlinkSync(brewReadsb.path);
    fs.symlinkSync(homeReadsb.path, brewReadsb.path);
    selection = await selectLocalAdsb('local:readsb', {fileSources: files, now: () => clock});
    assert.equal(selection.source.path, homeReadsb.path, 'Symlink candidates must not bypass file safety');
    fs.unlinkSync(brewReadsb.path);
    fs.unlinkSync(homeReadsb.path);

    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    const endpoint = `http://127.0.0.1:${server.address().port}`;
    const config = {capture: {fc: 100000000, replay: {state: false}},
      truth: {adsb: {enabled: true, tar1090: 'local:readsb', poll_interval: .1,
        smoothing_window: 3, max_position_age: 30}},
      location: {rx: {latitude: 51.4, longitude: -.5, altitude: 0},
        tx: {latitude: 51.5, longitude: -.6, altitude: 0}}};
    config.truth.adsb.tar1090 = 'auto';
    const source = createAdsbSource(config, {now: () => clock, discoveryCacheMs: 10000,
      discover: options => discoverLocalAdsb({...options, fileSources: files,
        httpSources: [{value: 'test-loopback', label: 'Local decoder', endpoint: endpoint + '/data/aircraft.json'}]})});
    assert.equal((await source.get('aircraft')).aircraft[0].hex, 'abc123');
    assert.equal((await source.status()).source.kind, 'http');
    clock += 200;
    payload = {...fresh(), now: (clock-60000)/1000};
    await assert.rejects(source.get('aircraft'), /stale/);
    clock += 200;
    payload = fresh();
    assert.equal((await source.status()).online, true, 'Discovery must recover when a decoder resumes fresh updates');
    assert.equal((await readJson(new URL(endpoint + '/data/aircraft.json'))).aircraft.length, 1);
    console.log('macOS ADS-B: Intel/ARM Homebrew + per-user files, safe selection, loopback HTTP, stale-feed rejection and recovery PASS (synthetic aircraft).');
  } finally {
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
