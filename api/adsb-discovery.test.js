'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {spawnSync} = require('child_process');
const {LOCAL_SOURCES, HTTP_SOURCES, SOURCE_CHOICES, classifyAdsbSource,
  withDeadline, readJsonFile, selectLocalAdsb, discoverLocalAdsb} = require('./adsb-discovery');

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'adsb-discovery-'));
  let clock = 100000;
  const write = (name, value) => {
    const filename = path.join(directory, name);
    fs.writeFileSync(filename, typeof value === 'string' ? value : JSON.stringify(value));
    return filename;
  };
  const candidate = (value, filename) => ({value, label: value, path: filename});
  try {
    assert.deepEqual(LOCAL_SOURCES.map(source => source.value), [
      'local:readsb', 'local:dump1090-fa', 'local:dump1090', 'local:dump1090-mutability']);
    assert.deepEqual(SOURCE_CHOICES.map(source => source.value),
      ['auto', 'local:readsb', 'local:dump1090-fa', 'local:dump1090',
        'local:dump1090-mutability']);
    assert.deepEqual(classifyAdsbSource('auto'), {mode: 'auto'});
    assert.equal(classifyAdsbSource('AUTO').mode, 'explicit', 'Only exact literal auto opts in');
    assert.equal(classifyAdsbSource(' auto ').mode, 'explicit', 'Whitespace is not reinterpreted');
    assert.equal(classifyAdsbSource('receiver.local:8080').mode, 'explicit');
    for (const source of HTTP_SOURCES) {
      const url = new URL(source.endpoint);
      assert.ok(['127.0.0.1', '[::1]'].includes(url.hostname), source.endpoint);
    }

    const readsb = write('readsb.json', {now: 100, aircraft: []});
    let httpProbes = 0;
    const fileSelection = await discoverLocalAdsb({
      fileSources: [candidate('local:readsb', readsb)],
      httpSources: [{value: 'test-http', label: 'test', endpoint: 'http://127.0.0.1/data/aircraft.json'}],
      fetchJson: async () => { httpProbes += 1; return {now: 100, aircraft: []}; },
      now: () => clock, maxAgeSeconds: 30
    });
    assert.equal(fileSelection.source.value, 'local:readsb');
    assert.equal(fileSelection.source.kind, 'file');
    assert.equal(httpProbes, 0, 'A unique decoder file wins without probing tar1090 HTTP');
    assert.deepEqual((await fileSelection.read()).aircraft, [], 'Empty aircraft is a healthy feed');

    const alias = path.join(directory, 'readsb-alias.json');
    fs.linkSync(readsb, alias);
    const deduplicated = await discoverLocalAdsb({
      fileSources: [candidate('local:readsb', readsb), candidate('local:dump1090', alias)],
      httpSources: [], now: () => clock, maxAgeSeconds: 30
    });
    assert.equal(deduplicated.source.value, 'local:readsb', 'One inode is one source');

    const second = write('dump1090.json', {now: 100, aircraft: []});
    await assert.rejects(discoverLocalAdsb({
      fileSources: [candidate('local:readsb', readsb), candidate('local:dump1090', second)],
      httpSources: [], now: () => clock, maxAgeSeconds: 30
    }), error => /Multiple local/.test(error.message) && error.sources.length === 2,
    'Distinct files require explicit selection even when their JSON is identical');

    const selected = await selectLocalAdsb('local:dump1090', {
      fileSources: [candidate('local:dump1090', second)], now: () => clock,
      maxAgeSeconds: 30
    });
    assert.equal(selected.source.path, second);
    await assert.rejects(selectLocalAdsb('local:other', {fileSources: []}), /Unknown/);

    const httpSelection = await discoverLocalAdsb({fileSources: [],
      httpSources: [
        {value: 'bad', label: 'bad', endpoint: 'http://127.0.0.1/bad/data/aircraft.json'},
        {value: 'good', label: 'good', endpoint: 'http://[::1]:8080/data/aircraft.json'}
      ], fetchJson: async url => {
        if (url.pathname.includes('/bad/')) throw new Error('offline');
        return {now: 100, aircraft: []};
      }, now: () => clock, maxAgeSeconds: 30});
    assert.equal(httpSelection.source.value, 'good');
    assert.equal(httpSelection.source.kind, 'http');

    const stale = write('stale.json', {now: 1, aircraft: []});
    await assert.rejects(discoverLocalAdsb({fileSources: [candidate('local:readsb', stale)],
      httpSources: [], now: () => clock, maxAgeSeconds: 30}), /No healthy/);
    const invalid = write('invalid.json', '<html>not json</html>');
    await assert.rejects(readJsonFile(invalid), /invalid JSON/);
    const truncated = write('truncated.json', '{"now":100,"aircraft":[');
    await assert.rejects(readJsonFile(truncated), /invalid JSON/);
    const large = write('large.json', 'x'.repeat(65));
    await assert.rejects(readJsonFile(large, {maxBytes: 64}), /size limit/);
    const fifo = path.join(directory, 'decoder.fifo');
    const madeFifo = spawnSync('mkfifo', [fifo], {encoding: 'utf8'});
    assert.equal(madeFifo.status, 0, madeFifo.stderr);
    await assert.rejects(readJsonFile(fifo, {timeoutMs: 50}), /not a regular file/);
    await assert.rejects(withDeadline(new Promise(() => {}), 10), /timed out/);
  } finally {
    fs.rmSync(directory, {recursive: true, force: true});
  }
  console.log('Local ADS-B discovery allowlist, freshness, precedence, ambiguity and bounds tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
