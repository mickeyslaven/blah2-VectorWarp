'use strict';

// UI tests live outside api/, while jsdom is intentionally an API dev
// dependency. Resolve it from the owning package rather than relying on the
// caller's cwd or a globally installed module.
module.exports = require('./api-require')('jsdom');
