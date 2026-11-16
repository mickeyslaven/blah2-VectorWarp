'use strict';

const path = require('path');
const fs = require('fs');
const {execFile} = require('child_process');

const unavailable = Object.freeze({schema: 1, portableGpu: false, residentGpu: false,
  widebandGpu: false, reason: 'native-capability-query-failed'});
const unavailableDriver = Object.freeze({version: 1, available: false,
  qualification: 'not-run', devices: [], error: 'gpu-driver-query-failed'});
const receiptFields = ['captureEpochExact', 'firstInputSampleExact', 'endInputSampleExact'];
function parseUint64(value) {
  if (typeof value !== 'string' || value.length > 20 || !/^(0|[1-9][0-9]*)$/.test(value)) return null;
  const integer = BigInt(value);
  return integer <= 18446744073709551615n ? integer : null;
}
function receiptInterval(receipt) {
  if (receipt?.known !== true || receipt.available !== true || receipt.frameBound !== true ||
      receipt.sampleInterval !== 'half-open' || receipt.provenance !== 'processor_stage_receipt') return null;
  const values = receiptFields.map(key => parseUint64(receipt[key]));
  return values.some(value => value === null) || values[2] <= values[1] ? null : values;
}

// Same exact-ID policy as GpuDeviceSelection.h. Enumeration is eligibility,
// not arithmetic qualification; never replace an explicit ineligible device.
function selectResidentGpuDevice(report, requested = 'auto') {
  const fail = reason => { throw new Error(reason); };
  if (!report || report.version !== 1 || report.available !== true ||
      report.qualification !== 'not-run' || !Array.isArray(report.devices) ||
      report.devices.length > 16) fail('gpu-driver-status-unavailable');
  const seen = new Set(), eligible = [];
  for (const device of report.devices) {
    if (!device || typeof device.id !== 'string' || !device.id || device.id.length >= 96 ||
        device.id.includes('\0') || seen.has(device.id) ||
        typeof device.name !== 'string' || !device.name || device.name.length > 256 ||
        typeof device.shaderFloat64 !== 'boolean') fail('invalid-gpu-device-report');
    seen.add(device.id);
    if (device.shaderFloat64) eligible.push(device.id);
  }
  eligible.sort();
  if (requested !== '' && requested !== 'auto') {
    if (typeof requested !== 'string' || !seen.has(requested)) fail('selected-gpu-device-unavailable');
    if (!eligible.includes(requested)) fail('selected-gpu-device-lacks-shaderFloat64');
    return requested;
  }
  if (!eligible.length) fail('no-device-with-required-features');
  return eligible[0];
}

function parseNativeProcessingCapabilities(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > 4096)
    throw new Error('Invalid native processing capability report.');
  const value = JSON.parse(text);
  if (!value || Array.isArray(value) || value.schema !== 1 ||
      typeof value.portableGpu !== 'boolean' ||
      typeof value.residentGpu !== 'boolean' ||
      typeof value.widebandGpu !== 'boolean' ||
      typeof value.reason !== 'string' || value.reason.length > 256 ||
      Object.keys(value).some(key =>
        !['schema', 'portableGpu', 'residentGpu', 'widebandGpu', 'reason'].includes(key)))
    throw new Error('Invalid native processing capability report.');
  return value;
}

function readNativeProcessingCapabilities(options = {}) {
  const executable = options.executable ||
    process.env.BLAH2_RECEIVER_STATUS_EXECUTABLE ||
    path.resolve(__dirname, '../bin/blah2');
  const run = options.execFile || execFile;
  return new Promise(resolve => run(executable, ['--processing-capabilities'], {
    timeout: 1500, maxBuffer: 4096, encoding: 'utf8', windowsHide: true,
    cwd: '/', env: {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C', LC_ALL: 'C'}
  }, (error, stdout) => {
    if (error) return resolve({...unavailable});
    try { resolve(parseNativeProcessingCapabilities(stdout)); }
    catch (_) { resolve({...unavailable}); }
  }));
}

function readGpuDriverStatus(options = {}) {
  const executable = options.executable ||
    process.env.BLAH2_RECEIVER_STATUS_EXECUTABLE || path.resolve(__dirname, '../bin/blah2');
  const worker = options.worker || path.join(path.dirname(executable), 'blah2-gpu-worker');
  const run = options.execFile || execFile;
  const env = {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C', LC_ALL: 'C'};
  if ((options.platform || process.platform) === 'darwin') {
    // The standalone driver is not installed in a system Vulkan search path.
    // Resolve it from this executable's runtime, never from ambient loader
    // variables, and reject symlinks/ancestor escapes from the expected path.
    try {
      const runtime = path.dirname(path.dirname(fs.realpathSync(executable)));
      const icd = path.join(runtime, 'share/vulkan/icd.d/MoltenVK_icd.json');
      if (fs.lstatSync(icd).isFile() && fs.realpathSync(icd) === icd) {
        env.VK_DRIVER_FILES = icd;
        env.VK_ICD_FILENAMES = icd;
      }
    } catch (_) { /* Non-standalone installs use normal system discovery. */ }
  }
  return new Promise(resolve => run(worker, ['--driver-status'], {
    timeout: 1500, maxBuffer: 32768, encoding: 'utf8', windowsHide: true,
    cwd: '/', env
  }, (error, stdout) => {
    try {
      if (typeof stdout !== 'string' || Buffer.byteLength(stdout) > 32768)
        throw new Error('Invalid GPU driver status');
      const value = JSON.parse(stdout);
      if (!value || value.version !== 1 || typeof value.available !== 'boolean' ||
          value.qualification !== 'not-run' || !Array.isArray(value.devices) ||
          value.devices.length > 16 || value.devices.some(device => !device ||
            typeof device.id !== 'string' || !device.id || device.id.length > 256 ||
            typeof device.name !== 'string' || !device.name || device.name.length > 256 ||
            typeof device.shaderFloat64 !== 'boolean') ||
          (!value.available && (value.devices.length ||
            typeof value.error !== 'string' || value.error.length > 4096)) ||
          (error && value.available))
        throw new Error('Invalid GPU driver status');
      resolve(value);
    } catch (_) { resolve({...unavailableDriver}); }
  }));
}

// Prefer native decimal receipts; never recover an exact identity by rounding
// a JSON number. Parsing is bounded before BigInt sees any untrusted text.
function exactCapturePosition(runtime) {
  const position = runtime.captureSamplePosition;
  if (position?.available !== true) return null;
  const fields = ['epoch', 'firstSample', 'endSample'];
  let exact = null;
  for (const telemetry of [runtime.acceleration, runtime.clutterAcceleration,
    runtime.widebandAcceleration]) {
    const receipt = telemetry?.execution;
    if (receipt === undefined || receipt?.known === false) continue;
    const values = receiptInterval(receipt);
    if (!values ||
        (exact && values.some((value, index) => value !== exact[index]))) return null;
    exact = values;
  }
  if (exact) {
    // Safe legacy fields are useful cross-checks. Unsafe ones carry no exact
    // identity and are not propagated as if they did.
    if (fields.some((key, index) => Number.isSafeInteger(position[key]) &&
        BigInt(position[key]) !== exact[index])) return null;
  } else {
    if (fields.some(key => !Number.isSafeInteger(position[key]) || position[key] < 0)) return null;
    exact = fields.map(key => BigInt(position[key]));
    if (exact[2] <= exact[1]) return null;
  }
  const result = {available: true};
  fields.forEach((key, index) => {
    result[key] = exact[index] <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(exact[index]) : null;
    result[`${key}Exact`] = exact[index].toString();
  });
  return result;
}

function enrichNativeProcessingCapabilities(report, options = {}) {
  const executable = options.executable ||
    process.env.BLAH2_RECEIVER_STATUS_EXECUTABLE || path.resolve(__dirname, '../bin/blah2');
  const directory = path.dirname(executable);
  const runtime = options.runtime || {};
  const devices = options.driverStatus?.available === true &&
    Array.isArray(options.driverStatus.devices) ? options.driverStatus.devices : [];
  const modulePresent = name => {
    try { return fs.statSync(path.join(directory, name)).isFile(); }
    catch (_) { return false; }
  };
  const worker = modulePresent('blah2-gpu-worker');
  const driverKnown = options.driverStatus?.available === true;
  let residentSelectionError = '';
  if (driverKnown) try {
    selectResidentGpuDevice(options.driverStatus,
      options.deviceSelector ?? process.env.BLAH2_GPU_DEVICE ?? 'auto');
  } catch (error) { residentSelectionError = error.message; }
  const position = exactCapturePosition(runtime);
  const frameBound = runtime.fresh === true && typeof runtime.processorGeneration === 'string' &&
    runtime.processorGeneration.length > 0 &&
    Number.isSafeInteger(runtime.nCpi) && runtime.nCpi > 0 && position !== null;
  const components = (acceptedBackends, acceptedStages) => {
    const result = [];
    for (const [name, telemetry] of [['ambiguity', runtime.acceleration],
      ['clutter', runtime.clutterAcceleration], ['wideband', runtime.widebandAcceleration]]) {
      if (!telemetry || !acceptedStages.includes(name)) continue;
      if (Array.isArray(telemetry.members) && telemetry.members.length) {
        if (!frameBound || telemetry.members.length > 256) continue;
        for (const member of telemetry.members) {
          if (!member || typeof member.device !== 'string' ||
              !member.device || member.device.length > 256 || typeof member.laneId !== 'string' ||
              !member.laneId || member.laneId.length > 128) continue;
          const interval = receiptInterval(member.execution);
          // A lane CPI can span preceding capture blocks, but must end in this
          // publication's capture block and retain its exact source epoch.
          if (!interval || interval[0] !== BigInt(position.epochExact) ||
              interval[2] <= BigInt(position.firstSampleExact) ||
              interval[2] > BigInt(position.endSampleExact)) continue;
          const selected = acceptedBackends.includes(member.backend);
          const executedBackend = member.executedBackend || member.backend;
          const executed = member.execution.gpuExecuted === true &&
            acceptedBackends.includes(executedBackend);
          if (!selected && !executed) continue;
          result.push({stage: name, laneId: member.laneId, backend: member.backend,
            device: member.device, state: member.state, reason: member.reason,
            selected, executedBackend: executed ? executedBackend : null,
            executed, execution: member.execution});
        }
        continue;
      }
      const selected = acceptedBackends.includes(telemetry.active);
      const executedBackend = telemetry.gpuExecutionBackend || telemetry.active;
      const executed = frameBound && telemetry.gpuExecuted === true &&
        (telemetry.execution === undefined || (telemetry.execution.known === true &&
          telemetry.execution.gpuExecuted === true)) && acceptedBackends.includes(executedBackend) &&
          typeof telemetry.device === 'string' && telemetry.device.length > 0;
      if (!selected && !executed) continue;
      result.push({stage: name, backend: telemetry.active, device: telemetry.device || null,
        state: telemetry.state, reason: telemetry.reason, selected,
        executedBackend: executed ? executedBackend : null, executed,
        execution: telemetry.execution || null});
    }
    return result;
  };
  const stage = (built, packaged, entries, eligible, driverLoaded = false,
    selectionError = '') => {
    const available = built && packaged;
    const selected = entries.some(entry => entry.selected);
    const executed = frameBound && entries.some(entry => entry.executed);
    const selectedDevices = [...new Set(entries.map(entry => entry.device).filter(Boolean))];
    const states = [...new Set(entries.map(entry => entry.state).filter(value => typeof value === 'string'))];
    const reason = [...new Set(entries.map(entry => entry.reason).filter(value => typeof value === 'string' && value))].join('; ');
    // Enumeration proves feature eligibility, not arithmetic qualification or
    // loading of the independent wideband/resident module. null means unknown.
    return {
      compiled: built, built, packaged,
      loadable: available ? (selected || executed || driverLoaded ? true : null) : false,
      discovered: available ? (selected || executed || driverKnown ? selected || executed || devices.length > 0 : null) : false,
      selectable: available && (selected || (eligible.length > 0 && !selectionError)),
      qualified: available && entries.length && frameBound && states.length ?
        states.length === 1 ? states[0] : 'mixed' : 'not-run',
      selected: available && selected,
      executed: available && executed,
      device: selectedDevices.length === 1 ? selectedDevices[0] : selectedDevices.length ? 'multiple' : null,
      devices: selectedDevices,
      executionMembers: available ? entries : [],
      eligibleDevices: available ? eligible.map(device => device.id) : [],
      reason: !built ? report.reason || 'stage-not-compiled' : !packaged ?
        'stage-module-not-packaged' : selected && !frameBound ? 'frame-lineage-unavailable' :
          entries.length ? reason :
          !selected && selectionError ? selectionError :
          driverKnown && eligible.length === 0 ? 'no-device-with-required-features' :
            !driverKnown ? options.driverStatus?.error || 'gpu-driver-status-unknown' : '',
    };
  };
  // Saved configuration can be pending restart. Only the actual frame backend
  // selects a route; it must not be relabelled by that future configuration.
  return {...report, frame: {
    bound: frameBound,
    processorGeneration: frameBound ? runtime.processorGeneration : null,
    nCpi: Number.isSafeInteger(runtime.nCpi) && runtime.nCpi > 0 ? runtime.nCpi : null,
    timestamp: Number.isSafeInteger(runtime.timestamp) ? runtime.timestamp : null,
    captureSamplePosition: frameBound ? {...position} : null,
  }, stages: {
    portable: stage(report.portableGpu === true,
      worker && modulePresent('blah2-gpu-vulkan.so'),
      components(['vulkan'], ['ambiguity', 'clutter']), devices, driverKnown),
    wideband: stage(report.widebandGpu === true,
      worker && modulePresent('blah2-gpu-wideband-vulkan.so'),
      components(['vulkan-wideband'], ['wideband']), devices),
    resident: stage(report.residentGpu === true,
      worker && modulePresent('blah2-gpu-resident-vulkan.so'),
      components(['vulkan-resident'], ['ambiguity', 'clutter']),
      devices.filter(device => device.shaderFloat64 === true), false, residentSelectionError),
  }};
}

module.exports = {parseNativeProcessingCapabilities,
  selectResidentGpuDevice,
  readNativeProcessingCapabilities, readGpuDriverStatus,
  enrichNativeProcessingCapabilities, unavailable, unavailableDriver};
