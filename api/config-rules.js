'use strict';

// Browser constraints and API scalar checks share one source of truth. Cross-field
// checks live in config-manager; these are processor limits, not radio detection.
const integer = (min, max) => ({type: 'number', integer: true, min, max});
const numeric = (min, max) => ({type: 'number', min, max});
const {SOURCE_CHOICES} = require('./adsb-discovery');
const rules = {
  'capture.fs': integer(1, 4294967295),
  'capture.fc': integer(1, 4294967295),
  'capture.device.channel_count': {...integer(2, 8), choices: [2,3,4,5,6,7,8]},
  'capture.device.reference_channel': integer(0, 7),
  'capture.device.heimdall.port': integer(1, 65535),
  'capture.device.heimdall.control_port': {...integer(1, 65535), optional: true},
  'capture.device.heimdall.gain': {type: 'gain', optional: true},
  'capture.device.agcSetPoint': integer(-72, 0),
  'capture.device.bandwidthNumber': {type: 'number', choices: [0,5,50,100]},
  // Published SDRplay API 3.09 section 5: RSPduo 50-ohm inputs, not Hi-Z.
  'capture.device.lnaState': {...integer(0, 9), choices: [0,1,2,3,4,5,6,7,8,9],
    frequencyBands: [{belowHz: 60000000, max: 6},
      {belowHz: 1000000000, max: 9}, {belowHz: 2000000001, max: 8}]},
  'capture.replay.format': {type: 'string', choices: ['auto', 'blah2', 'mchq', 's16-interleaved', 's8-interleaved', 'usrp-blocks'], optional: true},
  'capture.replay.legacy_block_samples': {...integer(1, 262144), optional: true},
  'process.data.cpi': {...numeric(), exclusiveMin: 0},
  'process.data.buffer': numeric(1),
  'process.data.overlap': {...numeric(0), readOnly: 'Not used by this processor.'},
  'process.detection.pfa': {...numeric(), exclusiveMin: 0, exclusiveMax: 1},
  'process.detection.nGuard': integer(0, 127),
  'process.detection.nTrain': integer(1, 127),
  'process.detection.nCentroid': integer(0, 65535),
  'process.detection.minDelay': integer(-128, 127),
  'process.detection.minDoppler': numeric(0),
  'process.tracker.initiate.M': integer(1, 255),
  'process.tracker.initiate.N': integer(1, 255),
  'process.tracker.initiate.maxAcc': numeric(0, 2147483647),
  'process.tracker.delete': integer(1, 255),
  'process.tracker.smooth': {type: 'string', readOnly: 'Not used by this processor.'},
  'process.performance.surveillance_workers': integer(0, 8),
  'process.performance.fft_threads': integer(0, 256),
  'process.performance.acceleration': {type: 'string', choices: ['auto', 'cpu', 'gpu']},
  'process.reference_synthesis.analysis_samples': integer(64, 4294967295),
  'process.reference_synthesis.analysis_interval': integer(1, 4294967295),
  'process.reference_synthesis.power_iterations': integer(1, 4294967295),
  'process.reference_synthesis.covariance_smoothing': {...numeric(0), exclusiveMax: 1},
  'process.reference_synthesis.diagonal_loading': numeric(0),
  'process.reference_synthesis.mode': {type: 'string', choices: ['array_eigenbeam', 'dedicated']},
  'truth.adsb.poll_interval': {...numeric(.1, 60), optional: true},
  'truth.adsb.smoothing_window': {...integer(2, 64), optional: true},
  'truth.adsb.max_position_age': {...numeric(.1, 300), optional: true},
  'truth.ais.port': {...integer(1, 65535), readOnly: 'AIS input is not implemented.'}
};
for (const group of ['ambiguity', 'clutter']) {
  for (const name of ['delayMin', 'delayMax', ...(group === 'ambiguity' ?
    ['dopplerMin', 'dopplerMax'] : [])])
    rules[`process.${group}.${name}`] = integer(-2147483648, 2147483647);
}
for (const name of ['api','map','detection','track','timestamp','timing','iqdata','config'])
  rules[`network.ports.${name}`] = {...integer(1, 65535), ...(name === 'config' ?
    {readOnly: 'Reserved; no configuration-control listener uses this port.'} : {})};
for (const site of ['rx','tx']) {
  rules[`location.${site}.latitude`] = numeric(-90, 90);
  rules[`location.${site}.longitude`] = numeric(-180, 180);
  rules[`location.${site}.altitude`] = numeric();
  rules[`location.${site}.name`] = {type: 'string'};
}
for (const name of ['capture.replay.state','capture.replay.loop',
  'capture.device.dabNotch','capture.device.rfNotch','process.clutter.enable',
  'process.detection.enable','process.tracker.enable','truth.adsb.enabled',
  'truth.ais.enabled','save.iq','save.map','save.detection','save.timing'])
  rules[name] = {type: 'boolean'};
for (const name of ['capture.device.type','capture.device.heimdall.host',
  'capture.device.address','capture.device.subdev','capture.device.serial','capture.replay.file',
  'network.ip','truth.adsb.tar1090','truth.ais.ip','save.path'])
  rules[name] = {type: 'string'};
for (const name of ['capture.device.surveillance_channels','capture.device.serial',
  'capture.device.antenna','capture.device.gain','capture.device.gain_lna',
  'capture.device.gain_vga','capture.device.gainReduction',
  'capture.device.amp_enable','process.reference_synthesis.channels'])
  rules[name] = {type: 'array'};
for (const name of ['truth.ais.enabled','truth.ais.ip','save.iq','save.timing']) rules[name].readOnly = 'Not used by this processor.';
rules['truth.adsb.tar1090'].sourceChoices = SOURCE_CHOICES;
rules['capture.device.serial'] = {type: 'serial'};

module.exports = rules;
