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
  'capture.device.heimdall.channel_gains': {type: 'array', optional: true},
  'capture.device.heimdall.gain_control': {type: 'string', choices: ['fixed', 'measured'], optional: true},
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
  'process.detection.integration_cpis': {...integer(1, 2), optional: true},
  'process.detection.integration_motion_bins': {...integer(0, 1), optional: true},
  'process.detection.track_before_detect.enable': {type: 'boolean', optional: true},
  'process.detection.track_before_detect.max_cpis': {...integer(4, 8), choices: [4,8], optional: true},
  'process.detection.track_before_detect.motion_bins': {...integer(0, 1), optional: true},
  'process.detection.track_before_detect.delay_motion_bins': {...integer(0, 2), optional: true},
  'process.detection.track_before_detect.doppler_motion_bins': {...integer(0, 16), optional: true},
  'process.bearing.enable': {type: 'boolean', optional: true},
  'process.bearing.min_consistent': {...integer(2, 100), optional: true},
  'process.bearing.min_peak_separation_db': {...numeric(1, 15), optional: true},
  'process.bearing.method': {type: 'string', choices: ['coherent_beam_bank', 'covariance_mvdr'], optional: true},
  'process.bearing.reference_anchor': {type: 'string', choices: ['live_eigenbeam', 'fixed_site', 'fixed_site_projected'], optional: true},
  'process.bearing.scan_beams': {...integer(4, 127), optional: true},
  'process.bearing.candidate_peaks': {...integer(3, 3), optional: true, readOnly: 'Fixed at three independent lobes.'},
  'process.bearing.guard_radius': {...integer(0, 3), optional: true},
  'process.bearing.training_radius': {...integer(1, 6), optional: true},
  'process.bearing.diagonal_loading': {...numeric(), exclusiveMin: 0, max: 1, optional: true},
  'process.bearing.max_rate_deg_s': {...numeric(0, 360), optional: true},
  'process.bearing.manifold_file': {type: 'string', optional: true},
  'process.bearing.manifold_automation.auto_promote': {type: 'boolean', optional: true},
  'process.bearing.channel_mapping': {type: 'array', optional: true},
  'process.bearing.frequency_resolved_shadow.enabled': {type: 'autoBoolean', choices: ['auto', 'on', 'off'], optional: true},
  'process.bearing.frequency_resolved_shadow.fft_size': {...integer(256, 16384), optional: true},
  'process.bearing.frequency_resolved_shadow.subbands': {...integer(4, 64), optional: true},
  'process.bearing.frequency_resolved_shadow.bandwidth_fraction': {...numeric(), exclusiveMin: 0, max: 1, optional: true},
  'process.bearing.frequency_resolved_shadow.diagonal_loading': {...numeric(), exclusiveMin: 0, max: 1, optional: true},
  'process.bearing.frequency_resolved_shadow.capture_seconds': {...numeric(.05, 10), optional: true},
  'process.bearing.frequency_resolved_shadow.interval_s': {...integer(10, 86400), optional: true},
  'process.bearing.frequency_resolved_shadow.timeout_ms': {...integer(1000, 60000), optional: true},
  'process.bearing.frequency_resolved_shadow.path': {type: 'string', optional: true},
  'process.bearing.manifold_recorder.enabled': {type: 'autoBoolean', choices: ['auto', 'on', 'off'], optional: true},
  'process.bearing.manifold_recorder.path': {type: 'string', optional: true},
  'process.bearing.manifold_recorder.min_snr_db': {...numeric(-100, 100), optional: true},
  'process.bearing.manifold_recorder.min_elevation_deg': {...numeric(-10, 90), optional: true},
  'process.bearing.manifold_recorder.max_elevation_deg': {...numeric(0, 90), optional: true},
  'process.bearing.manifold_recorder.max_delay_error_km': {...numeric(.01, 50), optional: true},
  'process.bearing.manifold_recorder.max_doppler_error_hz': {...numeric(.1, 500), optional: true},
  'process.bearing.manifold_recorder.max_truth_age_s': {...numeric(.1, 60), optional: true},
  'process.bearing.manifold_recorder.max_file_mb': {...integer(1, 1024), optional: true},
  'process.bearing.manifold_recorder.max_files': {...integer(1, 20), optional: true},
  'process.tracker.initiate.M': integer(1, 255),
  'process.tracker.initiate.N': integer(1, 255),
  'process.tracker.initiate.maxAcc': numeric(0, 2147483647),
  'process.tracker.initiate.acceleration_model': {type: 'string', choices: ['hypothesis_bank', 'adaptive_gate'], optional: true},
  'process.tracker.initiate.stationary_clutter.enable': {type: 'boolean', optional: true},
  'process.tracker.initiate.stationary_clutter.persistence_cpis': {...integer(2, 65535), optional: true},
  'process.tracker.initiate.stationary_clutter.max_doppler_hz': {...numeric(0, 10000), optional: true},
  'process.tracker.track_guided_shadow.enable': {type: 'boolean', optional: true},
  'process.tracker.track_guided_shadow.promote': {type: 'boolean', optional: true},
  'process.tracker.track_guided_shadow.max_tracks': {...integer(1, 65535), optional: true},
  'process.tracker.track_guided_shadow.evidence_path': {type: 'string', optional: true},
  'process.tracker.track_guided_shadow.max_truth_age_s': {...numeric(.1, 60), optional: true},
  'process.tracker.track_guided_shadow.max_file_mb': {...integer(1, 1024), optional: true},
  'process.tracker.track_guided_shadow.max_files': {...integer(1, 20), optional: true},
  'process.tracker.delete': integer(1, 255),
  // Storage count only; runtime checked admission enforces the GPU/shared caps.
  'process.tracker.gpu_capacity': {...integer(1, 4294967295), optional: true},
  'process.tracker.beam_tolerance': {...integer(0, 180), optional: true},
  'process.tracker.publication.mode': {type: 'string',
    choices: ['native', 'external', 'hybrid'], optional: true},
  'process.tracker.publication.event_file': {type: 'string', optional: true},
  'process.tracker.publication.stale_after_s': {...numeric(.1, 60), optional: true},
  'process.tracker.publication.native_min_associations': {...integer(1, 65535), optional: true},
  'process.tracker.publication.delay_tolerance': {...numeric(0, 1000), optional: true},
  'process.tracker.publication.doppler_tolerance': {...numeric(0, 10000), optional: true},
  'process.tracker.smooth': {type: 'string', readOnly: 'Not used by this processor.'},
  'process.performance.surveillance_workers': integer(0, 8),
  'process.performance.fft_threads': integer(0, 256),
  'process.performance.acceleration': {type: 'string', choices: ['auto', 'cpu', 'gpu', 'gpu-resident']},
  'process.reference_synthesis.analysis_samples': integer(64, 4294967295),
  'process.reference_synthesis.analysis_interval': integer(1, 4294967295),
  'process.reference_synthesis.power_iterations': integer(1, 4294967295),
  'process.reference_synthesis.covariance_smoothing': {...numeric(0), exclusiveMax: 1},
  'process.reference_synthesis.diagonal_loading': numeric(0),
  'process.reference_synthesis.mode': {type: 'string', choices: ['array_eigenbeam', 'dedicated']},
  'process.surveillance_fusion.mode': {type: 'string', choices: ['noncoherent', 'coherent_beam_bank', 'noise_whitened_beam_bank'], optional: true},
  'process.surveillance_fusion.beams': {...integer(4, 360), optional: true},
  'process.surveillance_fusion.effective_beam_trials': {...numeric(1, 360), optional: true},
  'process.surveillance_fusion.direct_path_null_db': {...numeric(0, 40), optional: true},
  'truth.adsb.poll_interval': {...numeric(.1, 60), optional: true},
  'truth.adsb.smoothing_window': {...integer(2, 64), optional: true},
  'truth.adsb.max_position_age': {...numeric(.1, 300), optional: true},
  'truth.adsb.source_timeout': {...numeric(.1, 10), optional: true,
    readOnly: 'Advanced multi-source setting; edit the YAML or configuration API.'},
  'truth.adsb.sources': {type: 'array', optional: true,
    readOnly: 'Advanced ordered source list; edit the YAML or configuration API.'},
  'truth.adsb.delay_tolerance': {...numeric(0, 1000), optional: true},
  'truth.adsb.doppler_tolerance': {...numeric(0, 10000), optional: true},
  'network.tracker_forward.enabled': {type: 'boolean', optional: true},
  'network.tracker_forward.host': {type: 'string', optional: true},
  'network.tracker_forward.port': {...integer(1, 65535), optional: true},
  'network.tracker_forward.annotate_adsb': {type: 'boolean', optional: true},
  'network.tracker_forward.unassisted.enabled': {type: 'boolean', optional: true},
  'network.tracker_forward.unassisted.host': {type: 'string', optional: true},
  'network.tracker_forward.unassisted.port': {...integer(1, 65535), optional: true},
  'truth.ais.port': {...integer(1, 65535), readOnly: 'AIS input is not implemented.'}
};
for (const group of ['ambiguity', 'clutter']) {
  for (const name of ['delayMin', 'delayMax', ...(group === 'ambiguity' ?
    ['dopplerMin', 'dopplerMax'] : [])])
    rules[`process.${group}.${name}`] = {
      ...integer(-2147483648, 2147483647),
      ...(group === 'clutter' ? {optional: true} : {})
    };
}
for (const name of ['api','map','detection','track','bearing','manifold','timestamp','timing','iqdata','config'])
  rules[`network.ports.${name}`] = {...integer(1, 65535), ...(name === 'config' ?
    {readOnly: 'Reserved; no configuration-control listener uses this port.'} :
    ['bearing','manifold'].includes(name) ? {optional: true} : {})};
for (const site of ['rx','tx']) {
  rules[`location.${site}.latitude`] = numeric(-90, 90);
  rules[`location.${site}.longitude`] = numeric(-180, 180);
  rules[`location.${site}.altitude`] = numeric();
  rules[`location.${site}.name`] = {type: 'string'};
}
for (const name of ['capture.replay.state','capture.replay.loop',
  'capture.device.dabNotch','capture.device.rfNotch','process.clutter.enable',
  'process.clutter.followAmbiguity',
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
