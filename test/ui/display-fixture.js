'use strict';
// Execute the production display contract in renderer VM tests, retaining only
// explicit boundary doubles (transport, clock, Plotly and scheduling).
const fs = require('node:fs');
const vm = require('node:vm');
module.exports = context => {
  const boundaries = {...context};
  vm.runInContext(fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8'), context);
  Object.assign(context, boundaries);
};
