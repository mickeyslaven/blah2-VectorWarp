'use strict';

const stash = require('./frame-history').timing();
module.exports = {get_data_timing: stash.get_data, update_data: stash.update_data};
