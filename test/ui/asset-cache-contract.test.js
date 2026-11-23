'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../../html');
const version = 'ui-consistency-32';
const assetVersions = {'error_reporting.js': 'reporting-4', 'transmitter_catalog.js': 'ui-consistency-37', 'insights.js': 'stats-policy-35', 'common.js': 'ui-consistency-37', 'blah2.css': 'ui-consistency-37',
  'plot_timing.js': 'ui-consistency-35', 'plot_spectrum.js': 'ui-consistency-35', 'consolidated_views.js': 'ui-consistency-37',
  'plot_map.js': 'ui-consistency-36', 'config_ui.js': 'ui-consistency-38', 'config_profiles.js': 'ui-consistency-37',
  'processing_capabilities.js': 'ui-consistency-37', 'setup_assistant.js': 'ui-consistency-37',
  'measured_track_coverage.js': 'ui-consistency-37',
  'fusion_aircraft.js': 'ui-consistency-33'};
// Changing a dynamic dependency also changes the loader URL at every entry point.
const assets = ['error_reporting.js', 'transmitter_catalog.js', 'common.js', 'insights.js', 'plot_map.js', 'blah2.css', 'consolidated_views.js',
  'plot_detection.js', 'plot_timing.js', 'plot_spectrum.js', 'recordings.js', 'config_ui.js', 'setup_assistant.js',
  'kraken_geometry.js', 'local_nodes.js', 'config_profiles.js', 'processing_capabilities.js', 'fusion_aircraft.js', 'fusion_scope.js', 'locations-map-layers.js', 'measured_track_coverage.js'];
const files = fs.readdirSync(root, {recursive:true})
  .filter(file => file.endsWith('.html') || file === 'js/consolidated_views.js');
const counts = Object.fromEntries(assets.map(asset => [asset, 0]));
for (const file of files) {
  const text = fs.readFileSync(path.join(root, file), 'utf8');
  for (const asset of assets) {
    const expression = new RegExp(`${asset.replaceAll('.', '\\.')}(?:\\?v=([\\w.-]+))?`, 'g');
    for (const match of text.matchAll(expression)) {
      assert.equal(match[1], assetVersions[asset] || version,
        `${file}: ${asset} must use its current release version`);
      counts[asset]++;
    }
  }
}
for (const [asset, count] of Object.entries(counts)) assert.ok(count > 0, `${asset} has checked consumers`);
const settings = fs.readFileSync(path.join(root, 'display/configuration/index.html'), 'utf8');
for (const dependent of ['transmitter_catalog.js', 'config_profiles.js', 'kraken_geometry.js', 'setup_assistant.js', 'config_ui.js'])
  assert.ok(settings.indexOf('processing_capabilities.js') < settings.indexOf(dependent),
    `Shared topology must load before ${dependent}`);
assert.ok(settings.indexOf('config_profiles.js') < settings.indexOf('setup_assistant.js'),
  'Shared numeric-unit conversion must load before the setup assistant');
const loader = fs.readFileSync(path.join(root, 'js/consolidated_views.js'), 'utf8');
for (const asset of ['insights.js', 'plot_map.js', 'plot_detection.js', 'plot_timing.js', 'plot_spectrum.js'])
  assert.ok(loader.includes(`${asset}?v=${assetVersions[asset] || version}`),
    `Dynamic ${asset} dependency is covered`);
const map = fs.readFileSync(path.join(root, 'display/locations/index.html'), 'utf8');
assert.doesNotMatch(map, /user-scalable=no|maximum-scale=1(?:,|\")/,
  'Map must allow browser zoom for readers who need larger text');
console.log(`Shared asset cache versions agree across ${files.length} route/loader files.`);
