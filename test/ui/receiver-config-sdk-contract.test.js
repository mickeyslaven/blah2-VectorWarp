'use strict';

// Source-level supplement to the runtime browser/config acceptance. It proves
// the persisted receiver keys are consumed by each compiled backend path; it
// is deliberately not hardware, driver, SDK-return, or readback evidence.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..', '..');
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const capture = read('src/capture/Capture.cpp');
const rsp = read('src/capture/rspduo/RspDuo.cpp');
const usrp = read('src/capture/usrp/Usrp.cpp');
const usrpReadback = read('src/capture/usrp/UsrpReadback.h');
const hackrf = read('src/capture/hackrf/HackRf.cpp');

for (const key of ['agcSetPoint', 'bandwidthNumber', 'gainReduction',
  'lnaState', 'dabNotch', 'rfNotch', 'address', 'subdev', 'antenna',
  'gain', 'serial', 'gain_lna', 'gain_vga', 'amp_enable'])
  assert.ok(capture.includes(`config["${key}"]`), `Capture factory omits ${key}`);
for (const call of ['multi_usrp::make(address)', 'set_rx_subdev_spec',
  'set_rx_antenna(antenna[0], 0)', 'set_rx_antenna(antenna[1], 1)',
  'set_rx_rate', 'set_rx_freq(centerFrequency, 0)',
  'set_rx_freq(centerFrequency, 1)', 'set_rx_gain(gain[0], 0)',
  'set_rx_gain(gain[1], 1)'])
  assert.ok(usrp.includes(call), `USRP startup omits ${call}`);
for (const call of ['hackrf_open_by_serial', 'hackrf_set_freq',
  'hackrf_set_sample_rate', 'hackrf_set_amp_enable', 'hackrf_set_lna_gain',
  'hackrf_set_vga_gain', 'hackrf_set_hw_sync_mode',
  'hackrf_set_clkout_enable'])
  assert.ok(hackrf.includes(call), `HackRF startup omits ${call}`);
for (const assignment of ['rfFreq.rfHz = fc', 'agc.setPoint_dBfs',
  'gain.gRdB = gain_reduction_nr_a', 'gain.gRdB = gain_reduction_nr_b',
  'gain.LNAstate = lna_state_nr', 'rfNotchEnable = rf_notch_fg',
  'rfDabNotchEnable = dab_notch_fg', 'decimationFactor = nDecimation',
  'bwType = bwType'])
  assert.ok(rsp.includes(assignment), `RSPduo startup omits ${assignment}`);

assert.match(hackrf, /void HackRf::check_status[\s\S]*throw std::runtime_error/);
assert.match(capture, /device->start\(\); device->process\(buffers\);[\s\S]*catch \(const std::exception& error\)[\s\S]*processing_error/);
assert.match(capture, /AddMember\("sampleRate",fs,a\)[\s\S]*AddMember\("frequency",fc,a\)/,
  'Processor status must remain classified as configured-value telemetry');

// UHD readback gates precede stream creation. This source contract is a guard
// against accidental removal, not a physical-device acceptance test.
for (const getter of ['get_rx_rate(', 'get_rx_freq(', 'get_rx_gain(',
  'get_rx_antenna(', 'get_rx_subdev_spec('])
  assert.equal(usrpReadback.includes(getter), true, `USRP is missing ${getter}`);
assert.ok(usrp.indexOf('verify_usrp_readback(*usrp') < usrp.indexOf('get_rx_stream(streamArgs)'));

// libhackrf reports success/failure for the current open/set/start calls. The
// path checks those codes, but does not independently query the applied RF
// values after setting them.
for (const call of ['hackrf_set_freq', 'hackrf_set_sample_rate',
  'hackrf_set_amp_enable', 'hackrf_set_lna_gain', 'hackrf_set_vga_gain'])
  assert.match(hackrf, new RegExp(`status = ${call}\\([^;]+;\\s*check_status\\(status,`),
    `HackRF does not check ${call}`);

const rspProcessExits = (rsp.match(/\bexit\(1\)/g) || []).length;
assert.equal(rspProcessExits, 0, 'RSPduo SDK failures must reach structured processor telemetry');
assert.match(rsp, /for \(auto\* channel : \{deviceParams->rxChannelA, deviceParams->rxChannelB\}\)/);
assert.match(rsp, /sdrplay_api_Init\(chosenDevice->dev, &cbFns, this\)/,
  'Callbacks must receive their live RspDuo instance');
assert.match(rsp, /cleanup_api\(\) noexcept/);
console.warn('PHYSICAL ACCEPTANCE PENDING: UHD getters are startup checks; SDRplay/HackRF success codes and parameter records do not independently verify actual RF, serial wiring or coherence.');
console.log('receiver config-to-SDK source contract passed (runtime hardware not claimed)');
