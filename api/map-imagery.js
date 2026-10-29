const fs = require('fs');
const path = require('path');

const DEFAULT_ROOT = '/var/lib/vectorwarp-api/map-imagery';

function readManifest(root) {
  try {
    const value = JSON.parse(fs.readFileSync(path.join(root, 'manifest.json'), 'utf8'));
    return {
      available: true,
      source: value.source || 'NAIP',
      label: value.label || 'Local aerial imagery',
      bounds: Array.isArray(value.bounds) ? value.bounds.slice(0, 4).map(Number) : null,
      minzoom: Number.isInteger(value.minzoom) ? value.minzoom : 0,
      maxzoom: Number.isInteger(value.maxzoom) ? value.maxzoom : 16,
      attribution: value.attribution || 'USGS, USDA, The National Map'
    };
  } catch (_) {
    return {available: false};
  }
}

function installMapImageryRoutes(app, {root = process.env.BLAH2_MAP_IMAGERY_ROOT || DEFAULT_ROOT} = {}) {
  app.get('/api/map/imagery', (_req, res) => res.json(readManifest(root)));
  app.get('/api/map/imagery/local/:z/:x/:y.jpg', (req, res) => {
    const values = ['z', 'x', 'y'].map(name => Number(req.params[name]));
    if (!values.every(Number.isInteger) || values.some(value => value < 0))
      return res.status(400).json({error: 'Invalid map tile coordinates.'});
    const [z, x, y] = values;
    if (z > 22 || x >= 2 ** z || y >= 2 ** z)
      return res.status(404).end();
    const filename = path.join(root, 'tiles', String(z), String(x), `${y}.jpg`);
    if (!fs.existsSync(filename)) return res.status(404).end();
    res.set('Cache-Control', 'public, max-age=31536000, immutable');
    res.type('image/jpeg').sendFile(filename);
  });
}

module.exports = {installMapImageryRoutes, readManifest};
