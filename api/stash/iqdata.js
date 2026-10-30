'use strict';

const stash = require('./frame-history').iqdata();
module.exports = {get_data_iqdata: stash.get_data, update_data: stash.update_data};
