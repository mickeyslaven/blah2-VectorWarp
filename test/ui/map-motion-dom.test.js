'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '../..');
const source = fs.readFileSync(path.join(root, 'html/js/insights.js'), 'utf8');
const page = fs.readFileSync(path.join(root, 'html/display/locations/index.html'), 'utf8');

assert.match(source, /if \(nextHeading !== null\) record\.heading =/,
  'aircraft retain their last valid ground track when a feed snapshot omits course');
assert.doesNotMatch(source, /record\.heading = heading;/,
  'a missing course cannot reset a rendered aircraft to north');
assert.match(source, /const mapBearing = finite\(map\.getBearing\?\.\(\)\) \?\? 0;/,
  'one camera bearing is sampled for every aircraft in a render frame');
assert.match(source, /updateNow - layer\._lastGeometryUpdate >= 90/,
  'expensive geographic overlays are throttled while the camera moves');
assert.match(source, /updateNow - layer\._lastLabelUpdate >= 120/,
  'label collision layout is throttled while the camera moves');
assert.match(source, /const endMapTransform = \(\) => layer\._schedulePositionUpdate\(true\)/,
  'camera movement always ends with an exact settled overlay pass');
assert.match(page, /insights\.js\?v=map-motion-1/,
  'the map requests the corrected motion renderer instead of a cached script');

console.log('Map orientation retention and camera-motion performance contracts passed.');
