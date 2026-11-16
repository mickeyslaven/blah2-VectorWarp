'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../../html');
const version = 'ui-consistency-29';
// Changing a dynamic dependency also changes the loader URL at every entry point.
const assets = ['common.js', 'insights.js', 'plot_map.js', 'blah2.css', 'consolidated_views.js',
  'plot_detection.js', 'recordings.js', 'config_ui.js', 'setup_assistant.js',
  'kraken_geometry.js', 'local_nodes.js', 'config_profiles.js', 'processing_capabilities.js', 'fusion_aircraft.js', 'fusion_scope.js'];
const files = fs.readdirSync(root, {recursive:true})
  .filter(file => file.endsWith('.html') || file === 'js/consolidated_views.js');
const counts = Object.fromEntries(assets.map(asset => [asset, 0]));
for (const file of files) {
  const text = fs.readFileSync(path.join(root, file), 'utf8');
  for (const asset of assets) {
    const expression = new RegExp(`${asset.replaceAll('.', '\\.')}(?:\\?v=([\\w.-]+))?`, 'g');
    for (const match of text.matchAll(expression)) {
      assert.equal(match[1], version, `${file}: ${asset} must use the shared release version`);
      counts[asset]++;
    }
  }
}
for (const [asset, count] of Object.entries(counts)) assert.ok(count > 0, `${asset} has checked consumers`);
const settings = fs.readFileSync(path.join(root, 'display/configuration/index.html'), 'utf8');
for (const dependent of ['config_profiles.js', 'kraken_geometry.js', 'setup_assistant.js', 'config_ui.js'])
  assert.ok(settings.indexOf('processing_capabilities.js') < settings.indexOf(dependent),
    `Shared topology must load before ${dependent}`);
const loader = fs.readFileSync(path.join(root, 'js/consolidated_views.js'), 'utf8');
for (const asset of ['insights.js', 'plot_map.js', 'plot_detection.js'])
  assert.ok(loader.includes(`${asset}?v=${version}`), `Dynamic ${asset} dependency is covered`);
const map = fs.readFileSync(path.join(root, 'display/locations/index.html'), 'utf8');
assert.doesNotMatch(map, /user-scalable=no|maximum-scale=1(?:,|\")/,
  'Map must allow browser zoom for readers who need larger text');
console.log(`Shared asset cache versions agree across ${files.length} route/loader files.`);
