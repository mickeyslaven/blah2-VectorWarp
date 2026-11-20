'use strict';

const assert = require('node:assert/strict');
const {createHash} = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const common = require('../../html/js/common.js');
const semantics = common.radarVisualSemantics;

assert.equal(semantics.aircraftColor, common.aircraftAltitudeColor);
assert.equal(semantics.aircraftSvgMarkup, common.aircraftSvgMarkup);
assert.notEqual(semantics.colors.selected, semantics.colors.support,
  'Selection must not claim radar support');
assert.notEqual(semantics.labels.adsb, semantics.labels.independent,
  'External position and independent radar position have distinct labels');
assert.deepEqual(semantics.legendOrder.map(kind => semantics.legendRank[kind]), [10, 20, 30, 40, 50]);
assert.equal(semantics.labels.multiChannel, 'Multi-channel support');
assert.equal(semantics.labels.adsbOnly, 'ADS-B only');
assert.equal(semantics.aircraftColor({}), '#a7b0ac', 'Unknown altitude remains neutral');
assert.notEqual(semantics.aircraftColor({alt_geom: 5000}), semantics.aircraftColor({alt_geom: 30000}));

// Preserve the existing regular Map icon silhouettes, depth, highlights, and
// neutral colors for every supported class. Shared use must not redesign them.
const aircraft = [{}, {category: 'A1'}, {category: 'A3'}, {category: 'A5'},
  {category: 'A7'}, {t: 'GLID'}, {category: 'B6'}, {category: 'C1'}];
const iconHash = createHash('sha256').update(aircraft.map(item =>
  common.aircraftSvgMarkup(item, common.aircraftAltitudeColor(item))).join('\n')).digest('hex');
assert.equal(iconHash, 'f4bcd8c078c2caba555c765fe82aca25435893c4d7223249860ec737a6119163',
  'Canonical regular Map aircraft artwork must remain unchanged');

// JS responses remain fresh in browser caches for an hour. The live page must
// invalidate both its new semantics dependency and the changed DD renderer.
const livePage = fs.readFileSync(path.join(__dirname, '../../html/index.html'), 'utf8');
assert.match(livePage, /common\.js\?v=ui-consistency-30/);
assert.match(livePage, /plot_map\.js\?v=ui-consistency-30/);

console.log('Visual semantics preserve Map aircraft icons and separate position, support, selection, and altitude.');
