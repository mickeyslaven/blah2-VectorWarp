'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const root = path.resolve(__dirname, '../..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const htmlFiles = directory => fs.readdirSync(path.join(root, directory), {withFileTypes: true})
  .flatMap(entry => entry.isDirectory() ? htmlFiles(path.join(directory, entry.name)) :
    entry.name.endsWith('.html') ? [path.join(directory, entry.name)] : []);

function rasterHasEdgeInk(file) {
  const image = fs.readFileSync(path.join(root, file));
  let offset = 8;
  let width, height;
  const idat = [];
  while (offset < image.length) {
    const length = image.readUInt32BE(offset);
    const type = image.toString('ascii', offset + 4, offset + 8);
    const data = image.subarray(offset + 8, offset + 8 + length);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      assert.equal(data[8], 8, 'favicon rasters must use canonical 8-bit channels');
      assert.equal(data[9], 6, 'favicon rasters must use canonical RGBA pixels');
    }
    if (type === 'IDAT') idat.push(data);
    offset += 12 + length;
  }
  const scanlines = zlib.inflateSync(Buffer.concat(idat));
  const rowBytes = width * 4 + 1;
  const black = pixel => pixel[0] < 24 && pixel[1] < 24 && pixel[2] < 24 && pixel[3] > 24;
  let previous = Buffer.alloc(width * 4);
  const paeth = (left, above, upperLeft) => {
    const estimate = left + above - upperLeft;
    const leftDelta = Math.abs(estimate - left);
    const aboveDelta = Math.abs(estimate - above);
    const upperLeftDelta = Math.abs(estimate - upperLeft);
    return leftDelta <= aboveDelta && leftDelta <= upperLeftDelta ? left :
      aboveDelta <= upperLeftDelta ? above : upperLeft;
  };
  for (let y = 0; y < height; y++) {
    const source = scanlines.subarray(y * rowBytes, (y + 1) * rowBytes);
    const row = Buffer.from(source.subarray(1));
    for (let i = 0; i < row.length; i++) {
      const left = i >= 4 ? row[i - 4] : 0;
      const above = previous[i];
      const upperLeft = i >= 4 ? previous[i - 4] : 0;
      const adjustment = source[0] === 1 ? left : source[0] === 2 ? above :
        source[0] === 3 ? Math.floor((left + above) / 2) :
          source[0] === 4 ? paeth(left, above, upperLeft) : 0;
      row[i] = (row[i] + adjustment) & 0xff;
    }
    if (black(row.subarray(0, 4)) || black(row.subarray(-4))) return true;
    if ((y === 0 || y === height - 1) && Array.from({length: width}, (_, x) =>
      black(row.subarray(x * 4, 4 + x * 4))).some(Boolean)) return true;
    previous = row;
  }
  return false;
}

const logo = read('html/favicon/vectorwarp-vw.svg');
assert.match(logo, /viewBox="0 0 128 128"/);
assert.match(logo, /fill="#ff8954"/);
assert.match(logo, /<rect[^>]+stroke="none"/);
assert.match(logo, /<path fill="#000"/);
assert.match(logo, /<path[^>]+stroke="none"/);
assert.match(logo, /VectorWarp VW logo/);
assert.match(logo, /M12 32H25/);
assert.match(logo, /Z M62 32H73/);
assert.doesNotMatch(logo.match(/d="([^"]+)"/)[1], /(?:^|[^0-9])12[89](?:[^0-9]|$)/,
  'The vector paths must stay inside the 128px canvas.');

const header = read('html/js/common.js');
assert.match(header, /src="\/favicon\/vectorwarp-vw\.svg" alt="VectorWarp"/);
assert.doesNotMatch(header, /alt="30hours"/);
assert.match(header, /function ensureBrandFavicon\(\)/);
assert.match(header, /favicon\.href = '\/favicon\/vectorwarp-vw\.svg'/);

const home = read('html/index.html');
assert.match(home, /rel="icon" href="\/favicon\/vectorwarp-vw\.svg" type="image\/svg\+xml"/);

for (const file of htmlFiles('html')) {
  const page = read(file);
  if (page.includes('js/common.js')) {
    assert.match(page, /js\/common\.js\?v=vw-logo-1/,
      `${file} must load the cache-busted VectorWarp header`);
  }
}

assert.equal(fs.realpathSync(path.join(root, 'html/favicon.ico')),
  path.join(root, 'html/favicon/favicon-32x32.png'));
assert.deepEqual(fs.readFileSync(path.join(root, 'html/favicon/favicon-32x32.png')).subarray(0, 8),
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
for (const size of [16, 32, 128, 196]) {
  assert.equal(rasterHasEdgeInk(`html/favicon/favicon-${size}x${size}.png`), false,
    `${size}px favicon ink must not touch the canvas edge`);
}

console.log('VectorWarp VW branding asset and references passed.');
