'use strict';

const stash = require('./frame-history').detection();
module.exports = {get_data_detection: stash.get_data, update_data: stash.update_data};
