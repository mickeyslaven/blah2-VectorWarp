const fs = require('fs');
const path = require('path');
const yaml = require('js-yaml');
const net = require('net');
const FIELD_RULES = require('./config-rules');
const {sourceAddress} = require('./adsb-source');

const KRAKEN_MAX_CHANNELS = 8;
const UINT32_MAX = 4294967295;
const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;
const DEVICE_PROFILES = [
  {
    type: 'Kraken',
    label: 'KrakenSDR Suite V2',
    description: '2–8 coherent channels streamed by HeIMDALL',
    sampleRate: 2400000,
    device: {
      type: 'Kraken',
      heimdall: {host: '127.0.0.1', port: 8091, control_port: 8092},
      channel_count: 5,
      reference_channel: 0,
      surveillance_channels: [0, 1, 2, 3, 4]
    },
    process: {
      performance: {surveillance_workers: 0, fft_threads: 0},
      reference_synthesis: {
        mode: 'array_eigenbeam',
        channels: [0, 1, 2, 3, 4],
        analysis_samples: 32768,
        analysis_interval: 10,
        power_iterations: 12,
        covariance_smoothing: 0.8,
        diagonal_loading: 0.001
      }
    }
  },
  {
    type: 'RspDuo',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'SDRplay RSPduo',
    description: 'Two coherent tuners in dual-tuner mode',
    sampleRate: 2000000,
    device: {
      type: 'RspDuo', agcSetPoint: -20, bandwidthNumber: 5,
      gainReduction: [50, 45], lnaState: 1,
      dabNotch: false, rfNotch: false
    }
  },
  {
    type: 'Usrp',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'Ettus USRP',
    description: 'Two coherent receive channels through UHD',
    sampleRate: 2000000,
    device: {
      type: 'Usrp', address: 'localhost', subdev: 'A:A A:B',
      antenna: ['RX2', 'RX2'], gain: [20, 20]
    }
  },
  {
    type: 'HackRF',
    process: {performance: {surveillance_workers: 0, fft_threads: 0}},
    label: 'Dual HackRF',
    description: 'Two synchronized HackRF receivers',
    sampleRate: 2000000,
    device: {
      type: 'HackRF',
      serial: ['REFERENCE_DEVICE_SERIAL_NUMBER',
        'SURVILLANCE_DEVICE_SERIAL_NUMBER'],
      gain_lna: [32, 32], gain_vga: [30, 30],
      amp_enable: [false, false]
    }
  }
];

function getDeviceProfiles() {
  return JSON.parse(JSON.stringify(DEVICE_PROFILES));
}

function isObject(value) {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function validateConfig(config, baseline = null) {
  const errors = [];
  const notices = [];
  const advise = (field, message) => notices.push({field, message});
  const inspectKeys = (value, name = 'configuration') => {
    if (Array.isArray(value)) {
      value.forEach((item, index) => inspectKeys(item, `${name}[${index}]`));
      return;
    }
    if (!isObject(value)) return;
    Object.entries(value).forEach(([key, child]) => {
      if (key === '__proto__' || key === 'prototype' || key === 'constructor')
        errors.push(`${name}.${key} is not allowed`);
      inspectKeys(child, `${name}.${key}`);
    });
  };
  const requireObject = (value, name) => {
    if (!isObject(value)) errors.push(`${name} must be an object`);
    return isObject(value) ? value : {};
  };
  const number = (value, name, options = {}) => {
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      errors.push(`${name} must be a finite number`);
      return null;
    }
    if (options.integer && !Number.isInteger(value))
      errors.push(`${name} must be an integer`);
    if (options.min !== undefined && value < options.min)
      errors.push(`${name} must be at least ${options.min}`);
    if (options.max !== undefined && value > options.max)
      errors.push(`${name} must be at most ${options.max}`);
    if (options.exclusiveMin !== undefined && value <= options.exclusiveMin)
      errors.push(`${name} must be greater than ${options.exclusiveMin}`);
    if (options.exclusiveMax !== undefined && value >= options.exclusiveMax)
      errors.push(`${name} must be smaller than ${options.exclusiveMax}`);
    return value;
  };
  const oneOf = (value, name, allowed) => {
    if (!allowed.includes(value))
      errors.push(`${name} must be one of ${allowed.join(', ')}`);
  };
  const boolean = (value, name) => {
    if (typeof value !== 'boolean') errors.push(`${name} must be true or false`);
  };
  const string = (value, name) => {
    if (typeof value !== 'string' || value.trim() === '')
      errors.push(`${name} must be a non-empty string`);
  };
  const integerArray = (value, name) => {
    if (!Array.isArray(value) || value.length === 0) {
      errors.push(`${name} must be a non-empty list`);
      return [];
    }
    value.forEach((item, index) => number(item, `${name}[${index}]`,
      {integer: true, min: 0}));
    if (new Set(value).size !== value.length)
      errors.push(`${name} must not contain duplicate channels`);
    return value;
  };

  const root = requireObject(config, 'configuration');
  inspectKeys(root);
  // Validate known fields independently of the current file's types. A broken
  // file must be repairable; an optional supported setting may be added.
  const inspectFields = (value, keys = []) => {
    const key = keys.join('.');
    // Former external converter address: ignored during migration and never
    // emitted by the in-process VectorWarp converter.
    if (key === 'truth.adsb.adsb2dd') return;
    const rule = FIELD_RULES[key];
    const previous = keys.reduce((item, name) => item?.[name], baseline);
    if (rule && (rule.type === 'array' ? !Array.isArray(value) : typeof value !== rule.type))
      errors.push(`${key} must be ${rule.type}`);
    if (isObject(value)) {
      if (key && !rule && !Object.keys(FIELD_RULES).some(field => field.startsWith(`${key}.`)) &&
          (!baseline || JSON.stringify(previous) !== JSON.stringify(value)))
        errors.push(`${key} is not a supported setting`);
      Object.entries(value).forEach(([name, child]) => inspectFields(child, [...keys, name]));
      return;
    }
    if (!rule) {
      if (!baseline || JSON.stringify(previous) !== JSON.stringify(value))
        errors.push(`${key} is not a supported setting`);
      return;
    }
    if (rule.type === 'number') number(value, key, rule);
    if (rule.choices) oneOf(value, key, rule.choices);
    if (rule.readOnly && baseline && previous !== undefined && previous !== value)
      errors.push(`${key} is read-only: ${rule.readOnly}`);
  };
  inspectFields(root);
  const capture = requireObject(root.capture, 'capture');
  const device = requireObject(capture.device, 'capture.device');
  const replay = requireObject(capture.replay, 'capture.replay');
  const processConfig = requireObject(root.process, 'process');
  const data = requireObject(processConfig.data, 'process.data');
  const ambiguity = requireObject(processConfig.ambiguity, 'process.ambiguity');
  const clutter = requireObject(processConfig.clutter, 'process.clutter');
  const detection = requireObject(processConfig.detection, 'process.detection');
  const tracker = requireObject(processConfig.tracker, 'process.tracker');
  const initiate = requireObject(tracker.initiate, 'process.tracker.initiate');
  const network = requireObject(root.network, 'network');
  const ports = requireObject(network.ports, 'network.ports');
  const location = requireObject(root.location, 'location');
  const receiver = requireObject(location.rx, 'location.rx');
  const transmitter = requireObject(location.tx, 'location.tx');
  const save = requireObject(root.save, 'save');

  number(capture.fs, 'capture.fs', {integer: true, min: 1, max: UINT32_MAX});
  number(capture.fc, 'capture.fc', {integer: true, min: 1, max: UINT32_MAX});
  string(device.type, 'capture.device.type');
  const profile = DEVICE_PROFILES.find(item => item.type === device.type);
  if (!profile) errors.push('capture.device.type is not supported');
  else {
    const allowed = new Set(Object.keys(profile.device));
    Object.keys(device).forEach(key => {
      if (!allowed.has(key))
        errors.push(`capture.device.${key} is not valid for ${device.type}`);
    });
    allowed.forEach(key => {
      if (!Object.prototype.hasOwnProperty.call(device, key))
        errors.push(`capture.device.${key} is required for ${device.type}`);
    });
  }
  boolean(replay.state, 'capture.replay.state');
  boolean(replay.loop, 'capture.replay.loop');
  string(replay.file, 'capture.replay.file');
  if (replay.format !== undefined)
    oneOf(replay.format, 'capture.replay.format', ['auto', 'blah2', 'mchq', 's16-interleaved', 's8-interleaved', 'usrp-blocks']);
  if (replay.legacy_block_samples !== undefined)
    number(replay.legacy_block_samples, 'capture.replay.legacy_block_samples', {integer: true, min: 1, max: 262144});
  if (replay.format === 'usrp-blocks' && replay.legacy_block_samples === undefined)
    errors.push('capture.replay.legacy_block_samples is required for usrp-blocks replay');

  if (device.type === 'Kraken') {
    const inferredChannelCount = device.channel_count ??
      (Array.isArray(device.gain) ? device.gain.length : undefined);
    const channelCount = number(inferredChannelCount,
      device.channel_count === undefined ? 'capture.device.gain channel count' :
        'capture.device.channel_count', {integer: true, min: 2,
          max: KRAKEN_MAX_CHANNELS});
    const referenceChannel = number(device.reference_channel,
      'capture.device.reference_channel', {integer: true, min: 0});
    const surveillance = integerArray(device.surveillance_channels,
      'capture.device.surveillance_channels');
    const synthesis = requireObject(processConfig.reference_synthesis,
      'process.reference_synthesis');
    if (!['dedicated', 'array_eigenbeam'].includes(synthesis.mode))
      errors.push('process.reference_synthesis.mode must be dedicated or array_eigenbeam');
    const referenceChannels = synthesis.channels === undefined ? [] :
      integerArray(synthesis.channels, 'process.reference_synthesis.channels');
    if (channelCount !== null) {
      if (referenceChannel !== null && referenceChannel >= channelCount)
        errors.push('capture.device.reference_channel is outside channel_count');
      [...surveillance, ...referenceChannels].forEach(channel => {
        if (Number.isInteger(channel) && channel >= channelCount)
          errors.push(`capture.device.channel_count: input ${channel} is outside the configured channel count`);
      });
      if (synthesis.mode === 'dedicated' &&
          surveillance.includes(referenceChannel))
        errors.push('capture.device.reference_channel cannot also be a surveillance channel in dedicated mode');
      if (synthesis.mode === 'array_eigenbeam' && referenceChannels.length === 1)
        errors.push('process.reference_synthesis.channels requires at least two reference channels in array_eigenbeam mode');
    }
    if (capture.fs !== 2400000)
      errors.push('KrakenSDR Suite V2 requires capture.fs to be 2400000');
    if (capture.fc < 24000000 || capture.fc > 1766000000)
      advise('capture.fc', 'Outside the standard tuner range. Check upstream tuning.');
    const heimdall = requireObject(device.heimdall,
      'capture.device.heimdall');
    string(heimdall.host, 'capture.device.heimdall.host');
    number(heimdall.port, 'capture.device.heimdall.port',
      {integer: true, min: 1, max: 65535});
    if (heimdall.control_port !== undefined) {
      number(heimdall.control_port, 'capture.device.heimdall.control_port',
        {integer: true, min: 1, max: 65535});
      if (heimdall.control_port === heimdall.port)
        errors.push('capture.device.heimdall.control_port must differ from the IQ data port');
    }
    if (typeof heimdall.host === 'string' &&
        !net.isIP(heimdall.host) && !/^(?=.{1,253}$)[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?$/i.test(heimdall.host))
      errors.push('capture.device.heimdall.host must be an IP address or hostname, without a URL or port');
  } else if (device.type === 'Usrp') {
    string(device.address, 'capture.device.address');
    string(device.subdev, 'capture.device.subdev');
    if (!Array.isArray(device.antenna) || device.antenna.length !== 2)
      errors.push('capture.device.antenna must contain exactly two values');
    if (!Array.isArray(device.gain) || device.gain.length !== 2)
      errors.push('capture.device.gain must contain exactly two values');
    else device.gain.forEach((value, index) =>
      number(value, `capture.device.gain[${index}]`));
    if (Array.isArray(device.antenna)) device.antenna.forEach((value, index) =>
      string(value, `capture.device.antenna[${index}]`));
  } else if (device.type === 'HackRF') {
    if (capture.fs < 2000000 || capture.fs > 20000000)
      errors.push('capture.fs must be between 2 and 20 MS/s for HackRF');
    if (capture.fc < 1000000)
      errors.push('capture.fc must be at least 1 MHz for HackRF');
    ['serial', 'gain_lna', 'gain_vga', 'amp_enable'].forEach(key => {
      if (!Array.isArray(device[key]) || device[key].length !== 2)
        errors.push(`capture.device.${key} must contain exactly two values`);
    });
    if (Array.isArray(device.serial)) {
      device.serial.forEach((value, index) => {
        string(value, `capture.device.serial[${index}]`);
        if (typeof value !== 'string' || !/^[a-f0-9]{1,32}$/i.test(value))
          errors.push(`capture.device.serial[${index}] must be the device's hexadecimal serial number (up to 32 characters)`);
      });
      if (device.serial[0]?.toLowerCase?.() === device.serial[1]?.toLowerCase?.())
        errors.push('capture.device.serial must identify two different HackRF receivers');
    }
    if (Array.isArray(device.gain_lna))
      device.gain_lna.forEach((value, index) => {
        number(value, `capture.device.gain_lna[${index}]`,
          {integer: true, min: 0, max: 40});
        if (Number.isInteger(value) && value % 8 !== 0)
          errors.push(`capture.device.gain_lna[${index}] must use an 8 dB step`);
      });
    if (Array.isArray(device.gain_vga))
      device.gain_vga.forEach((value, index) => {
        number(value, `capture.device.gain_vga[${index}]`,
          {integer: true, min: 0, max: 62});
        if (Number.isInteger(value) && value % 2 !== 0)
          errors.push(`capture.device.gain_vga[${index}] must use a 2 dB step`);
      });
    if (Array.isArray(device.amp_enable))
      device.amp_enable.forEach((value, index) =>
        boolean(value, `capture.device.amp_enable[${index}]`));
  } else if (device.type === 'RspDuo') {
    number(capture.fc, 'capture.fc', {integer: true, min: 1,
      max: 2000000000});
    oneOf(capture.fs, 'capture.fs',
      [62500, 125000, 250000, 500000, 1000000, 2000000]);
    number(device.agcSetPoint, 'capture.device.agcSetPoint',
      {integer: true, min: -72, max: 0});
    number(device.bandwidthNumber, 'capture.device.bandwidthNumber',
      {integer: true, min: 0});
    oneOf(device.bandwidthNumber, 'capture.device.bandwidthNumber',
      [0, 5, 50, 100]);
    if (!Array.isArray(device.gainReduction) ||
        device.gainReduction.length !== 2)
      errors.push('capture.device.gainReduction must contain exactly two values');
    else device.gainReduction.forEach((value, index) =>
      number(value, `capture.device.gainReduction[${index}]`,
        {integer: true, min: 20, max: 59}));
    number(device.lnaState, 'capture.device.lnaState',
      {integer: true, min: 1, max: 9});
    boolean(device.dabNotch, 'capture.device.dabNotch');
    boolean(device.rfNotch, 'capture.device.rfNotch');
  }

  const cpi = number(data.cpi, 'process.data.cpi', {exclusiveMin: 0});
  const buffer = number(data.buffer, 'process.data.buffer', {min: 1});
  number(data.overlap, 'process.data.overlap', {min: 0});
  const sampleRate = Number.isFinite(capture.fs) ? capture.fs : null;
  const cpiSamples = cpi !== null && sampleRate !== null ?
    Math.floor(cpi * sampleRate) : null;
  if (cpiSamples !== null && (cpiSamples < 2000 || cpiSamples > INT32_MAX))
    errors.push('process.data.cpi must produce 2,000–2,147,483,647 samples for the spectrum and FFT processors');
  if (cpi !== null && buffer !== null && sampleRate !== null &&
      cpi * buffer * sampleRate > UINT32_MAX)
    errors.push('process.data.buffer creates an unsupported capture buffer');

  const delayMin = number(ambiguity.delayMin, 'process.ambiguity.delayMin',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  const delayMax = number(ambiguity.delayMax, 'process.ambiguity.delayMax',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  const dopplerMin = number(ambiguity.dopplerMin,
    'process.ambiguity.dopplerMin',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  const dopplerMax = number(ambiguity.dopplerMax,
    'process.ambiguity.dopplerMax',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  if (delayMin !== null && delayMax !== null && delayMin >= delayMax)
    errors.push('process.ambiguity.delayMin must be smaller than process.ambiguity.delayMax');
  if (dopplerMin !== null && dopplerMax !== null && dopplerMin >= dopplerMax)
    errors.push('process.ambiguity.dopplerMin must be smaller than process.ambiguity.dopplerMax');
  const delayBins = delayMin !== null && delayMax !== null ?
    delayMax - delayMin + 1 : null;
  if (delayBins !== null && (delayBins < 2 || delayBins > 65534))
    errors.push('process.ambiguity.delayMax: delay span must contain 2–65534 bins');
  if (delayMin !== null && delayMin > 1)
    errors.push('process.ambiguity.delayMin must be 1 or smaller');
  if (delayMax !== null && delayMax < -1)
    errors.push('process.ambiguity.delayMax must be -1 or larger');
  let ambiguityCpi = null;
  if (cpi !== null && cpiSamples !== null && cpiSamples > 0 &&
      dopplerMin !== null && dopplerMax !== null && dopplerMin < dopplerMax) {
    const middle = (dopplerMin + dopplerMax) / 2;
    if (dopplerMin + dopplerMax < INT32_MIN || dopplerMin + dopplerMax > INT32_MAX)
      errors.push('process.ambiguity.dopplerMax: combined Doppler limits overflow the processor');
    const positiveSteps = Math.max(0,
      Math.floor((dopplerMax - middle) * (cpiSamples / sampleRate)));
    const dopplerBins = 1 + 2 * positiveSteps;
    if (dopplerBins > cpiSamples || dopplerBins > 65535)
      errors.push('process.ambiguity.dopplerMax: Doppler span creates too many bins for this CPI');
    else {
      const correlationSamples = Math.floor(cpiSamples / dopplerBins);
      ambiguityCpi = correlationSamples * dopplerBins / sampleRate;
      // FFT rounding matches next_hamming (factors 2, 3 and 5). Never loop
      // through user-sized ranges: only bounded candidate lengths are searched.
      const required = 2 * correlationSamples - 1;
      let fft = Infinity;
      for (let a = 1; a <= 65535; a *= 2)
        for (let b = a; b <= 65535; b *= 3)
          for (let n = b; n <= 65535; n *= 5)
            if (n > required) fft = Math.min(fft, n);
      if (!Number.isFinite(fft))
        errors.push('process.ambiguity.dopplerMax: widen the Doppler span; correlation FFT exceeds the processor limit');
      if (delayBins !== null && delayBins >= fft)
        errors.push('process.ambiguity.delayMax: reduce the delay span or narrow the Doppler span');
    }
  }
  boolean(clutter.enable, 'process.clutter.enable');
  const clutterMin = number(clutter.delayMin, 'process.clutter.delayMin',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  const clutterMax = number(clutter.delayMax, 'process.clutter.delayMax',
    {integer: true, min: INT32_MIN, max: INT32_MAX});
  if (clutterMin !== null && clutterMax !== null && clutterMin >= clutterMax)
    errors.push('process.clutter.delayMin must be smaller than process.clutter.delayMax');
  if (clutterMin !== null && clutterMax !== null && cpiSamples !== null &&
      clutterMax - clutterMin > cpiSamples)
    errors.push('process.clutter.delayMax: filter delay span must fit inside the CPI sample count');
  if (clutterMax - clutterMin + cpiSamples + 1 > INT32_MAX)
    errors.push('process.clutter.delayMax: filter FFT exceeds the processor limit');
  const channels = device.type === 'Kraken' ? device.channel_count : 2;
  const paths = device.type === 'Kraken' ? device.surveillance_channels?.length : 1;
  // Lower-bound estimate only: FFT plans, allocator overhead and other services
  // need additional memory. Deployment-specific budgets may be set by the host.
  const estimatedBytes = 16 * (channels * cpiSamples * buffer +
    (channels + paths + 1) * cpiSamples +
    paths * (9 * cpiSamples + (clutterMax - clutterMin) ** 2));
  const memoryBudget = Number(process.env.BLAH2_CONFIG_MEMORY_LIMIT_MB || 0) * 1048576;
  if (memoryBudget > 0 && estimatedBytes > memoryBudget)
    errors.push('process.data.cpi: estimated radar memory exceeds this installation’s configured budget');
  boolean(detection.enable, 'process.detection.enable');
  number(detection.pfa, 'process.detection.pfa',
    {exclusiveMin: 0, exclusiveMax: 1});
  ['nGuard', 'nTrain'].forEach(key =>
    number(detection[key], `process.detection.${key}`,
      {integer: true, min: key === 'nGuard' ? 0 : 1, max: 127}));
  number(detection.nCentroid, 'process.detection.nCentroid',
    {integer: true, min: 0, max: 65535});
  number(detection.minDelay, 'process.detection.minDelay',
    {integer: true, min: -128, max: 127});
  number(detection.minDoppler, 'process.detection.minDoppler', {min: 0});
  if (delayBins !== null && Number.isInteger(detection.nGuard) &&
      Number.isInteger(detection.nTrain) &&
      2 * detection.nGuard + 2 >= delayBins)
    errors.push('process.detection.nGuard: reduce guard cells or widen the delay span so noise cells remain');
  if (delayMin !== null && delayMax !== null &&
      Number.isInteger(detection.minDelay) &&
      (detection.minDelay < delayMin || detection.minDelay > delayMax))
    errors.push('process.detection.minDelay must be inside the ambiguity delay span');
  if (dopplerMin !== null && dopplerMax !== null &&
      Number.isFinite(detection.minDoppler) &&
      detection.minDoppler > Math.max(Math.abs(dopplerMin), Math.abs(dopplerMax)))
    errors.push('process.detection.minDoppler must be inside the Doppler span');
  boolean(tracker.enable, 'process.tracker.enable');
  ['M', 'N'].forEach(key => number(initiate[key],
    `process.tracker.initiate.${key}`, {integer: true, min: 1, max: 255}));
  if (Number.isInteger(initiate.M) && Number.isInteger(initiate.N) &&
      initiate.M > initiate.N)
    errors.push('process.tracker.initiate.M cannot exceed process.tracker.initiate.N');
  const maxAcc = number(initiate.maxAcc,
    'process.tracker.initiate.maxAcc', {min: 0, max: INT32_MAX});
  if (maxAcc !== null && ambiguityCpi &&
      maxAcc * ambiguityCpi * ambiguityCpi > 65535)
    errors.push('process.tracker.initiate.maxAcc is too large for this CPI');
  number(tracker.delete, 'process.tracker.delete',
    {integer: true, min: 1, max: 255});
  string(tracker.smooth, 'process.tracker.smooth');
  if (processConfig.performance !== undefined) {
    const performance = requireObject(processConfig.performance,
      'process.performance');
    if (performance.surveillance_workers !== undefined) number(performance.surveillance_workers,
      'process.performance.surveillance_workers',
      {integer: true, min: 0, max: KRAKEN_MAX_CHANNELS});
    if (performance.fft_threads !== undefined) number(performance.fft_threads, 'process.performance.fft_threads',
      {integer: true, min: 0, max: 256});
  }
  if (processConfig.reference_synthesis !== undefined) {
    const synthesis = requireObject(processConfig.reference_synthesis,
      'process.reference_synthesis');
    number(synthesis.analysis_samples,
      'process.reference_synthesis.analysis_samples',
      {integer: true, min: 64, max: UINT32_MAX});
    ['analysis_interval', 'power_iterations'].forEach(key =>
      number(synthesis[key], `process.reference_synthesis.${key}`,
        {integer: true, min: 1, max: UINT32_MAX}));
    number(synthesis.covariance_smoothing,
      'process.reference_synthesis.covariance_smoothing',
      {min: 0, exclusiveMax: 1});
    number(synthesis.diagonal_loading,
      'process.reference_synthesis.diagonal_loading', {min: 0});
    if (synthesis.power_iterations > 100)
      advise('process.reference_synthesis.power_iterations', 'May slow updates. Try 12 iterations.');
  }

  string(network.ip, 'network.ip');
  if (typeof network.ip === 'string' && net.isIP(network.ip) !== 4)
    errors.push('network.ip must be an IPv4 address; the processor recording-control client does not support IPv6 URLs');
  for (const name of ['api','map','detection','track','timestamp','timing','iqdata','config'])
    number(ports[name], `network.ports.${name}`, {integer: true, min: 1, max: 65535});
  const portValues = [];
  Object.entries(ports).forEach(([name, value]) => {
    number(value, `network.ports.${name}`,
      {integer: true, min: 1, max: 65535});
    portValues.push(value);
  });
  if (new Set(portValues).size !== portValues.length)
    Object.entries(ports).forEach(([name, value]) => {
      if (portValues.filter(port => port === value).length > 1)
        errors.push(`network.ports.${name}: network ports must be unique`);
    });

  for (const [name, site] of [['rx', receiver], ['tx', transmitter]]) {
    number(site.latitude, `location.${name}.latitude`, {min: -90, max: 90});
    number(site.longitude, `location.${name}.longitude`,
      {min: -180, max: 180});
    number(site.altitude, `location.${name}.altitude`);
    string(site.name, `location.${name}.name`);
  }
  const truth = requireObject(root.truth, 'truth');
  if (receiver.latitude === transmitter.latitude && receiver.longitude === transmitter.longitude)
    advise('location', 'Set separate receiver and transmitter locations to use the map.');
  const adsb = requireObject(truth.adsb, 'truth.adsb');
  const ais = requireObject(truth.ais, 'truth.ais');
  boolean(adsb.enabled, 'truth.adsb.enabled');
  string(adsb.tar1090, 'truth.adsb.tar1090');
  for (const name of ['tar1090']) {
    try { sourceAddress(adsb[name]); }
    catch (_) { errors.push(`truth.adsb.${name}: use a hostname, host:port or HTTP(S) address without credentials or a query`); }
  }
  if (adsb.poll_interval !== undefined) number(adsb.poll_interval, 'truth.adsb.poll_interval', {min: .1, max: 60});
  if (adsb.smoothing_window !== undefined) number(adsb.smoothing_window, 'truth.adsb.smoothing_window', {integer: true, min: 2, max: 64});
  if (adsb.max_position_age !== undefined) number(adsb.max_position_age, 'truth.adsb.max_position_age', {min: .1, max: 300});
  boolean(ais.enabled, 'truth.ais.enabled');
  string(ais.ip, 'truth.ais.ip');
  number(ais.port, 'truth.ais.port', {integer: true, min: 1, max: 65535});
  ['iq', 'map', 'detection', 'timing'].forEach(key =>
    boolean(save[key], `save.${key}`));
  string(save.path, 'save.path');
  if (typeof save.path === 'string' && (!path.isAbsolute(save.path) || !save.path.endsWith('/')))
    errors.push('save.path must be a full directory path ending in /');
  return {valid: errors.length === 0, errors: [...new Set(errors)], notices,
    warnings: notices.map(notice => notice.message),
    estimatedMemoryBytes: Number.isFinite(estimatedBytes) ? estimatedBytes : null};
}

function writeConfigAtomically(filename, config) {
  const directory = path.dirname(filename);
  const temporary = path.join(directory,
    `.${path.basename(filename)}.${process.pid}.${Date.now()}.tmp`);
  const backup = `${filename}.bak`;
  const output = yaml.dump(config, {noRefs: true, lineWidth: 100,
    quotingType: '"', forceQuotes: false});
  try {
    const previous = fs.existsSync(filename) ? fs.statSync(filename) : null;
    fs.writeFileSync(temporary, output, {mode: previous ? previous.mode & 0o777 : 0o640, flag: 'wx'});
    if (previous) {
      fs.chownSync(temporary, previous.uid, previous.gid);
      fs.chmodSync(temporary, previous.mode & 0o777);
    }
    const descriptor = fs.openSync(temporary, 'r');
    fs.fsyncSync(descriptor);
    fs.closeSync(descriptor);
    if (fs.existsSync(filename)) fs.copyFileSync(filename, backup);
    fs.renameSync(temporary, filename);
    const directoryDescriptor = fs.openSync(directory, 'r');
    fs.fsyncSync(directoryDescriptor);
    fs.closeSync(directoryDescriptor);
  } finally {
    if (fs.existsSync(temporary)) fs.unlinkSync(temporary);
  }
}

module.exports = {KRAKEN_MAX_CHANNELS, getDeviceProfiles, validateConfig, FIELD_RULES,
  writeConfigAtomically};
