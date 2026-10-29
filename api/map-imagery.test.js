const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {readManifest} = require('./map-imagery');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-map-imagery-'));
try {
  assert.deepEqual(readManifest(root), {available: false});
  fs.writeFileSync(path.join(root, 'manifest.json'), JSON.stringify({
    source: 'NAIP', label: 'Pittsburgh aerial', bounds: [-81, 40, -79, 41], minzoom: 6, maxzoom: 16
  }));
  assert.deepEqual(readManifest(root), {
    available: true, source: 'NAIP', label: 'Pittsburgh aerial',
    bounds: [-81, 40, -79, 41], minzoom: 6, maxzoom: 16,
    attribution: 'USGS, USDA, The National Map'
  });
  console.log('map imagery manifest tests passed');
} finally { fs.rmSync(root, {recursive: true, force: true}); }
