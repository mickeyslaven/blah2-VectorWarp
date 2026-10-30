'use strict';

const stash = require('./frame-history').maxhold();
module.exports = {get_data_map: stash.get_data, update_data: stash.update_data};
