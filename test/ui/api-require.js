'use strict';

const path = require('path');
const {createRequire} = require('module');

module.exports = createRequire(path.resolve(__dirname, '../../api/package.json'));
