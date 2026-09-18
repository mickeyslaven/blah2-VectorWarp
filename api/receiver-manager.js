'use strict';

const net = require('net');

// This module deliberately has no child_process, network, systemd or package-
// manager imports. Host observation is supplied by a narrow read-only adapter;
// setup is a plan only.

const RECEIVER_TYPES = Object.freeze(['Kraken', 'RspDuo', 'Usrp', 'HackRF']);
const MAX_DOCUMENT_BYTES = 262144;
const MAX_USB_DEVICES = 64;
const MAX_TEXT = 160;
const DEFAULT_TIMEOUT_MS = 1000;

const DEFINITIONS = Object.freeze({
  Kraken: Object.freeze({
    label: 'KrakenSDR Suite V2',
    dependency: 'kraken-suite-v2',
    sourceSupported: true,
    direct: false,
    service: 'kraken-suite-v2'
  }),
  RspDuo: Object.freeze({
    label: 'SDRplay RSPduo',
    dependency: 'sdrplay-api-3.15',
    sourceSupported: true,
    direct: true,
    service: 'sdrplay-api'
  }),
  Usrp: Object.freeze({
    label: 'Ettus USRP / UHD',
    dependency: 'uhd-4.1',
    sourceSupported: true,
    direct: true,
    service: null
  }),
  HackRF: Object.freeze({
    label: 'Dual HackRF',
    dependency: 'libhackrf',
    sourceSupported: true,
    direct: true,
    service: null
  })
});

// Sync is field-specific. In particular, Suite V2 reports its compiled sample
// rate but no runtime sample-rate setter has been verified. The inspected TCP
// status does not advertise command capabilities, so mappings distinguish its
// fixed source contract from actual feature negotiation.
const SETTINGS_SYNC = Object.freeze({
  Kraken: Object.freeze([
    Object.freeze({configField: 'capture.fc', authority: 'upstream',
      statusField: 'settings.center_freq', controlOperation: 'set_frequency',
      requiredCapability: null, capabilityAdvertised: false,
      commandContract: 'inspected-suite-v2-newline-json',
      direction: 'browser-to-upstream-after-ack-and-readback'}),
    Object.freeze({configField: 'capture.fs', authority: 'upstream',
      statusField: 'settings.sample_rate', controlOperation: null,
      requiredCapability: null, direction: 'upstream-authoritative-mismatch-block',
      reason: 'Suite V2 reports a compile/startup sample rate; no runtime setter is verified.'}),
    Object.freeze({configField: 'capture.device.channel_count', authority: 'upstream',
      statusField: 'num_channels', controlOperation: 'set_num_elements',
      requiredCapability: null, capabilityAdvertised: false,
      commandContract: 'inspected-suite-v2-newline-json',
      direction: 'browser-to-upstream-after-ack-and-readback'}),
    Object.freeze({configField: 'capture.device.heimdall', authority: 'vectorwarp',
      direction: 'config-only'}),
    Object.freeze({configField: 'capture.device.reference_channel', authority: 'vectorwarp',
      direction: 'config-only'}),
    Object.freeze({configField: 'capture.device.surveillance_channels', authority: 'vectorwarp',
      direction: 'config-only'}),
    Object.freeze({configField: 'process.reference_synthesis', authority: 'vectorwarp',
      direction: 'config-only'})
  ]),
  RspDuo: Object.freeze([
    'capture.fc', 'capture.fs', 'capture.device.agcSetPoint',
    'capture.device.bandwidthNumber', 'capture.device.gainReduction',
    'capture.device.lnaState', 'capture.device.dabNotch',
    'capture.device.rfNotch'
  ].map(configField => Object.freeze({configField, authority: 'vectorwarp',
    direction: 'direct-tuning', confirmation: 'unsupported-readback',
    hardwareReadback: false}))),
  Usrp: Object.freeze([
    'capture.fc', 'capture.fs', 'capture.device.address',
    'capture.device.subdev', 'capture.device.antenna', 'capture.device.gain'
  ].map(configField => Object.freeze({configField, authority: 'vectorwarp',
    direction: 'direct-tuning', confirmation: 'unsupported-readback',
    hardwareReadback: false}))),
  HackRF: Object.freeze([
    'capture.fc', 'capture.fs', 'capture.device.serial',
    'capture.device.gain_lna', 'capture.device.gain_vga',
    'capture.device.amp_enable'
  ].map(configField => Object.freeze({configField, authority: 'vectorwarp',
    direction: 'direct-tuning', confirmation: 'unsupported-readback',
    hardwareReadback: false})))
});

function plainObject(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const prototype = Object.getPrototypeOf(value);
  return prototype === Object.prototype || prototype === null;
}

function exactKeys(value, allowed, name) {
  if (!plainObject(value)) throw inputError(`${name} must be an object.`);
  for (const key of Object.keys(value)) {
    if (!allowed.includes(key)) throw inputError(`${name}.${key} is not supported.`);
  }
}

function inputError(message) {
  const error = new Error(message);
  error.code = 'INVALID_RECEIVER_REQUEST';
  error.status = 422;
  return error;
}

function safeText(value, name, optional = false) {
  if (optional && (value === undefined || value === null || value === '')) return null;
  if (typeof value !== 'string' || value.length === 0 || value.length > MAX_TEXT ||
      /[\u0000-\u001f\u007f]/.test(value))
    throw new Error(`${name} is invalid.`);
  return value;
}

function safeJsonSize(value, name) {
  let encoded;
  try { encoded = JSON.stringify(value); }
  catch (_) { throw inputError(`${name} must not contain cycles.`); }
  if (encoded === undefined || Buffer.byteLength(encoded) > MAX_DOCUMENT_BYTES)
    throw inputError(`${name} exceeds 256 KiB.`);
}

function validateDiscoveryInput(input) {
  exactKeys(input, ['config', 'compiledLiveTypes'], 'request');
  if (!plainObject(input.config)) throw inputError('request.config must be an object.');
  safeJsonSize(input.config, 'request.config');
  if (!Array.isArray(input.compiledLiveTypes) ||
      input.compiledLiveTypes.some(type => !RECEIVER_TYPES.includes(type)) ||
      new Set(input.compiledLiveTypes).size !== input.compiledLiveTypes.length)
    throw inputError('request.compiledLiveTypes must be a unique list of supported receiver types.');
  const configuredType = input.config.capture?.device?.type;
  if (configuredType !== undefined && !RECEIVER_TYPES.includes(configuredType))
    throw inputError('request.config.capture.device.type is not supported.');
  return {config: input.config,
    compiledLiveTypes: new Set(input.compiledLiveTypes), configuredType: configuredType || null};
}

function normalizeTimeout(timeoutMs) {
  if (timeoutMs === undefined) return DEFAULT_TIMEOUT_MS;
  if (!Number.isInteger(timeoutMs) || timeoutMs < 10 || timeoutMs > 5000)
    throw inputError('timeoutMs must be an integer from 10 through 5000.');
  return timeoutMs;
}

function errorRecord(code, scope, message) {
  const cleaned = String(message || 'Probe failed.').replace(/[\r\n\t]+/g, ' ').slice(0, 240);
  return {code, scope, message: cleaned, retryable: code === 'PROBE_TIMEOUT' || code === 'PROBE_FAILED'};
}

async function runProbe(name, fn, argument, timeoutMs, errors, fallback) {
  if (typeof fn !== 'function') {
    errors.push(errorRecord('PROBE_UNAVAILABLE', name,
      `No read-only ${name} probe is configured.`));
    return fallback;
  }
  const controller = new AbortController();
  let timer;
  try {
    return await Promise.race([
      Promise.resolve().then(() => fn(argument, {signal: controller.signal,
        timeoutMs})),
      new Promise((_, reject) => {
        timer = setTimeout(() => {
          controller.abort();
          const error = new Error(`${name} probe exceeded ${timeoutMs} ms.`);
          error.code = 'PROBE_TIMEOUT';
          reject(error);
        }, timeoutMs);
      })
    ]);
  } catch (error) {
    errors.push(errorRecord(error.code === 'PROBE_TIMEOUT' ? 'PROBE_TIMEOUT' :
      'PROBE_FAILED', name, error.message));
    return fallback;
  } finally {
    clearTimeout(timer);
  }
}

function normalizeUsbInventory(value) {
  if (!Array.isArray(value) || value.length > MAX_USB_DEVICES)
    throw new Error(`USB inventory must contain at most ${MAX_USB_DEVICES} devices.`);
  return value.map((device, index) => {
    exactKeys(device, ['manufacturer', 'product', 'serial'], `usb[${index}]`);
    return {
      manufacturer: safeText(device.manufacturer, `usb[${index}].manufacturer`, true),
      product: safeText(device.product, `usb[${index}].product`, true),
      // Serial is accepted for exact configured-device correlation but is not
      // returned in discovery evidence.
      serial: safeText(device.serial, `usb[${index}].serial`, true)
    };
  });
}

function usbMatches(type, device) {
  const product = device.product || '';
  const manufacturer = device.manufacturer || '';
  if (type === 'RspDuo') return /\brspduo\b/i.test(product) &&
    /sdrplay/i.test(manufacturer);
  if (type === 'HackRF') return /\bhackrf one\b/i.test(product) &&
    /great scott gadgets/i.test(manufacturer);
  if (type === 'Usrp') return /\busrp\b/i.test(product) &&
    /ettus|national instruments/i.test(manufacturer);
  // Kraken contains RTL-derived USB devices whose generic descriptors are not
  // a unique Kraken identity. It is detected only through configured upstream
  // Suite telemetry.
  return false;
}

function normalizeDependencies(value) {
  if (!plainObject(value)) throw new Error('Dependency inventory must be an object.');
  const result = {};
  for (const type of RECEIVER_TYPES) {
    const expected = DEFINITIONS[type].dependency;
    const item = value[type];
    if (item === undefined) {
      result[type] = {state: 'unknown', installed: [], missing: [], unknown: [expected]};
      continue;
    }
    exactKeys(item, ['state', 'version'], `dependencies.${type}`);
    if (!['installed', 'missing', 'unknown'].includes(item.state))
      throw new Error(`dependencies.${type}.state is invalid.`);
    result[type] = {state: item.state,
      installed: item.state === 'installed' ? [expected] : [],
      missing: item.state === 'missing' ? [expected] : [],
      unknown: item.state === 'unknown' ? [expected] : [],
      ...(item.version === undefined ? {} :
        {version: safeText(item.version, `dependencies.${type}.version`)})};
  }
  return result;
}

function configuredUsbMatches(type, config, matches) {
  if (config.capture?.device?.type !== type) return {matches, identityMatched: null};
  const device = config.capture.device;
  if (type === 'HackRF') {
    const normalize = value => typeof value === 'string' && /^[a-f0-9]{1,32}$/i.test(value) ?
      value.toLowerCase().padStart(32, '0') : null;
    const wanted = Array.isArray(device.serial) ? device.serial.map(normalize) : [];
    const identities = matches.map(item => normalize(item.serial));
    const exact = wanted.length === 2 && wanted.every(Boolean) && new Set(wanted).size === 2 &&
      wanted.every(serial => identities.filter(value => value === serial).length === 1);
    return {matches: exact ? matches.filter(item => wanted.includes(normalize(item.serial))) : [],
      identityMatched: exact};
  }
  if (type === 'Usrp') {
    if (usrpLocality(device.address) !== 'local') return {matches: [], identityMatched: false};
    const serial = /(?:^|,)\s*serial\s*=\s*([^,]+)/i.exec(device.address || '')?.[1]?.trim();
    if (serial) {
      const exact = matches.filter(item => item.serial === serial);
      return {matches: exact.length === 1 ? exact : [], identityMatched: exact.length === 1};
    }
  }
  if (type === 'RspDuo') {
    const serial = typeof config.capture?.device?.serial === 'string' ? config.capture.device.serial : null;
    if (serial) {
      const exact = matches.filter(item => item.serial === serial);
      return {matches: exact.length === 1 ? exact : [], identityMatched: exact.length === 1};
    }
    return {matches: matches.length === 1 ? matches : [], identityMatched: matches.length === 1 ? null : false};
  }
  return {matches, identityMatched: null};
}

function isLoopback(host) {
  if (typeof host !== 'string') return false;
  const value = host.trim().toLowerCase().replace(/^\[|\]$/g, '');
  if (value === 'localhost' || value === '::1') return true;
  return net.isIP(value) === 4 && Number(value.split('.')[0]) === 127;
}

function usrpLocality(address) {
  if (typeof address !== 'string') return 'unknown';
  const match = address.match(/(?:^|,)\s*addr\s*=\s*([^,]+)/i);
  const target = match ? match[1].trim() : address.trim();
  if (isLoopback(target)) return 'local';
  if (match || net.isIP(target)) return 'remote';
  if (/(?:^|,)\s*(?:type|serial|resource)\s*=\s*[^,]+/i.test(address) &&
      !/(?:^|,)\s*addr\s*=/i.test(address)) return 'local';
  return 'unknown';
}

function localityFor(type, configured, config) {
  if (type === 'Kraken') {
    if (!configured) return 'unknown';
    const host = config.capture?.device?.heimdall?.host;
    return typeof host === 'string' ? (isLoopback(host) ? 'local' : 'remote') : 'unknown';
  }
  if (type === 'Usrp' && configured)
    return usrpLocality(config.capture?.device?.address);
  return 'local';
}

function configuredKrakenEndpoint(config) {
  const heimdall = config.capture?.device?.heimdall;
  if (!plainObject(heimdall)) return null;
  const host = heimdall.host;
  const dataPort = heimdall.port;
  const controlPort = heimdall.control_port ?? 8092;
  if (typeof host !== 'string' || host.length < 1 || host.length > 253 ||
      /[\s/@?#]/.test(host) || !Number.isInteger(dataPort) || dataPort < 1 ||
      dataPort > 65535 || !Number.isInteger(controlPort) || controlPort < 1 ||
      controlPort > 65535) return null;
  return {host, dataPort, controlPort};
}

function normalizeUpstream(value) {
  if (!plainObject(value) || typeof value.available !== 'boolean')
    throw new Error('Configured upstream status must report available as true or false.');
  const capabilities = value.capabilities === undefined ? [] : value.capabilities;
  if (!Array.isArray(capabilities) || capabilities.length > 32 ||
      capabilities.some(item => typeof item !== 'string' || item.length > 64))
    throw new Error('Configured upstream capabilities are invalid.');
  return {availability: value.available ? 'available' : 'unavailable',
    capabilities: [...new Set(capabilities)],
    ...(typeof value.matched === 'boolean' ? {settingsMatched: value.matched} : {}),
    ...(value.message ? {message: safeText(value.message, 'upstream.message')} : {})};
}

function normalizeService(value) {
  if (!plainObject(value)) throw new Error('Service status must be an object.');
  exactKeys(value, ['state'], 'service');
  if (!['running', 'stopped', 'unknown'].includes(value.state))
    throw new Error('service.state is invalid.');
  return value.state;
}

function normalizeNativeReceiverStatus(value) {
  if (!plainObject(value)) throw new Error('Native receiver status must be an object.');
  const result = {};
  for (const type of RECEIVER_TYPES) {
    const item = value[type];
    if (!plainObject(item)) throw new Error(`Native receiver status for ${type} is invalid.`);
    exactKeys(item, ['builtIn', 'compiled', 'moduleLoadable', 'localBuildable', 'error'], `nativeStatus.${type}`);
    if (typeof item.builtIn !== 'boolean' || typeof item.compiled !== 'boolean' ||
        typeof item.moduleLoadable !== 'boolean' ||
        (item.localBuildable !== undefined && typeof item.localBuildable !== 'boolean') ||
        typeof item.error !== 'string' ||
        item.error.length > 240 || /[\u0000-\u001f\u007f]/.test(item.error) ||
        (!item.compiled && item.moduleLoadable))
      throw new Error(`Native receiver status for ${type} is invalid.`);
    result[type] = {builtIn: item.builtIn, compiled: item.compiled,
      moduleLoadable: item.moduleLoadable, localBuildable: item.localBuildable === true,
      error: item.error};
  }
  return result;
}

function getSettingsMapping(type) {
  if (!RECEIVER_TYPES.includes(type)) throw inputError('receiver type is not supported.');
  return SETTINGS_SYNC[type].map(item => ({...item}));
}

function progress(state, message) {
  return {state, message};
}

function planReceiverSetup(request, discovery) {
  exactKeys(request, ['receiverType', 'locality'], 'request');
  if (!RECEIVER_TYPES.includes(request.receiverType))
    throw inputError('request.receiverType is not supported.');
  if (request.locality !== undefined && !['local', 'remote'].includes(request.locality))
    throw inputError('request.locality must be local or remote.');
  if (!plainObject(discovery) || discovery.schemaVersion !== 1 ||
      !Array.isArray(discovery.receivers))
    throw inputError('discovery must be a receiver-manager schemaVersion 1 result.');
  const receiver = discovery.receivers.find(item => item.type === request.receiverType);
  if (!receiver) throw inputError('discovery does not contain the selected receiver.');
  const locality = request.locality || receiver.locality;
  if (!['local', 'remote'].includes(locality))
    throw inputError('request.locality is required when discovery locality is unknown.');
  if (locality === 'remote' && ['RspDuo', 'HackRF'].includes(request.receiverType))
    throw inputError(`${request.receiverType} remote capture is not supported.`);
  const actions = [];
  const errors = [];
  const add = (id, kind, target, status, execution, message) => actions.push({
    id, kind, target, status, execution, message,
    progress: progress(status === 'not-required' ? 'complete' :
      status === 'blocked' ? 'blocked' : 'pending', message)
  });

  const localBuildAvailable = request.receiverType === 'RspDuo' &&
    receiver.capabilities.localBuildable === true;
  if (!receiver.capabilities.liveCompiled && !localBuildAvailable) {
    add('backend', 'provide-live-backend', request.receiverType, 'blocked',
      'unsupported', 'This VectorWarp build does not support the selected receiver.');
    errors.push(errorRecord('BACKEND_NOT_COMPILED', request.receiverType,
      'Install a reviewed build containing this receiver backend.'));
  } else if (!receiver.capabilities.liveCompiled) {
    add('backend', 'build-local-backend', request.receiverType, 'blocked',
      'explicit-local-build', 'The local RSPduo source kit is available, but support is not built. Build SDRplay support, then check receiver software again.');
    errors.push(errorRecord('LOCAL_BUILD_REQUIRED', request.receiverType,
      'The RSPduo adapter must be built locally before it can be used for live capture.'));
  } else if (receiver.capabilities.runtimeLoadable === false) {
    add('backend', 'provide-live-backend', request.receiverType, 'blocked',
      'runtime-unavailable', 'Receiver support is built, but its required runtime software is unavailable.');
    errors.push(errorRecord('RUNTIME_MODULE_UNAVAILABLE', request.receiverType,
      receiver.capabilities.runtimeError || 'Receiver support could not load its runtime software.'));
  } else add('backend', 'provide-live-backend', request.receiverType,
    'not-required', 'none', 'Receiver support and required runtime software are ready.');

  if (receiver.dependencies.state === 'installed')
    add('dependency', 'install-dependency', DEFINITIONS[request.receiverType].dependency,
      'not-required', 'none', 'The required receiver software is installed.');
  else if (request.receiverType === 'Kraken' && locality === 'remote') {
    add('dependency', 'verify-dependency', DEFINITIONS.Kraken.dependency,
      'blocked', 'unsupported',
      'Suite software is on the remote receiver host. It cannot be installed or checked locally.');
    errors.push(errorRecord('REMOTE_RIGHTS_REQUIRED', 'Kraken',
      'VectorWarp does not manage software on the remote Suite host.'));
  }
  else {
    const proprietary = request.receiverType === 'RspDuo';
    add('dependency', 'install-dependency', DEFINITIONS[request.receiverType].dependency,
      'blocked', 'unsupported', proprietary ?
        'You must obtain and accept the SDRplay license before installing its API.' :
        'No approved administrator setup is available for this software.');
    errors.push(errorRecord(proprietary ? 'LICENSE_ACCEPTANCE_REQUIRED' :
      'INSTALL_ADAPTER_UNAVAILABLE', request.receiverType,
    proprietary ? 'VectorWarp cannot accept the SDRplay license for you.' :
      'Install this required software outside VectorWarp.'));
  }

  add('configure', 'configure-receiver', request.receiverType, 'required',
    'unprivileged-existing-config-api',
    'Review receiver, tuning, channel, and site settings before saving.');

  if (request.receiverType === 'Kraken') {
    if (locality === 'remote') {
      if (receiver.upstream.availability === 'available')
        add('upstream-service', 'ensure-upstream-running', 'kraken-suite-v2',
          'not-required', 'none', 'The remote Suite endpoint is available.');
      else {
        add('upstream-service', 'ensure-upstream-running', 'kraken-suite-v2',
          'blocked', 'unsupported', 'You need separate permission to control services on that host.');
        if (!errors.some(error => error.code === 'REMOTE_RIGHTS_REQUIRED'))
          errors.push(errorRecord('REMOTE_RIGHTS_REQUIRED', 'Kraken',
            'VectorWarp does not manage services on the remote Suite host.'));
      }
    } else if (receiver.managedService.state === 'running') {
      add('upstream-service', 'ensure-upstream-running', 'kraken-suite-v2',
        'not-required', 'none', 'The local Suite service is running.');
    } else {
      add('upstream-service', 'ensure-upstream-running', 'kraken-suite-v2',
        'blocked', 'unsupported',
        'VectorWarp does not manage the Suite service and cannot control it.');
      errors.push(errorRecord('EXTERNAL_SERVICE_NOT_MANAGED', 'Kraken',
        'Review the local Suite installation and service owner.'));
    }
  } else if (request.receiverType === 'RspDuo') {
    if (receiver.managedService.state === 'running')
      add('upstream-service', 'ensure-upstream-running', 'sdrplay-api',
        'not-required', 'none', 'The local SDRplay API service is running.');
    else {
      add('upstream-service', 'ensure-upstream-running', 'sdrplay-api',
        'blocked', 'unsupported',
        'VectorWarp does not manage the vendor API service.');
      if (!errors.some(error => error.code === 'LICENSE_ACCEPTANCE_REQUIRED'))
        errors.push(errorRecord('EXTERNAL_SERVICE_NOT_MANAGED', 'RspDuo',
          'Review the vendor API installation and service status.'));
    }
  } else add('upstream-service', 'ensure-upstream-running', request.receiverType,
    'not-required', 'none', 'This receiver has no separate service.');

  add('verify', 'verify-readiness', request.receiverType, 'required',
    'read-only', 'Run discovery again and check radar status before live use.');
  return {schemaVersion: 1, receiverType: request.receiverType, locality,
    executable: false, actions, errors,
    progressStates: ['pending', 'running', 'complete', 'failed', 'blocked']};
}

function createReceiverManager(options = {}) {
  exactKeys(options, ['probes', 'timeoutMs', 'now'], 'options');
  const probes = options.probes === undefined ? {} : options.probes;
  exactKeys(probes, ['usbInventory', 'dependencyInventory', 'nativeReceiverStatus',
    'configuredUpstreamStatus', 'serviceStatus'], 'options.probes');
  const timeoutMs = normalizeTimeout(options.timeoutMs);
  const now = options.now === undefined ? Date.now : options.now;
  if (typeof now !== 'function') throw inputError('options.now must be a function.');

  async function discover(input) {
    const request = validateDiscoveryInput(input);
    const errors = [];
    let usb = await runProbe('usb-inventory', probes.usbInventory, {},
      timeoutMs, errors, null);
    let usbKnown = false;
    if (usb !== null) {
      try { usb = normalizeUsbInventory(usb); usbKnown = true; }
      catch (error) {
        errors.push(errorRecord('INVALID_PROBE_RESULT', 'usb-inventory', error.message));
        usb = [];
      }
    } else usb = [];
    let dependencies = await runProbe('dependency-inventory',
      probes.dependencyInventory, {}, timeoutMs, errors, {});
    try { dependencies = normalizeDependencies(dependencies); }
    catch (error) {
      errors.push(errorRecord('INVALID_PROBE_RESULT', 'dependency-inventory', error.message));
      dependencies = normalizeDependencies({});
    }
    let nativeStatus = null;
    if (typeof probes.nativeReceiverStatus === 'function') {
      const rawNativeStatus = await runProbe('native-receiver-status', probes.nativeReceiverStatus,
        {}, timeoutMs, errors, null);
      if (rawNativeStatus !== null) {
        try { nativeStatus = normalizeNativeReceiverStatus(rawNativeStatus); }
        catch (error) {
          errors.push(errorRecord('INVALID_PROBE_RESULT', 'native-receiver-status', error.message));
        }
      }
    }

    let upstream = {availability: 'unknown', capabilities: []};
    const serviceStates = Object.fromEntries(RECEIVER_TYPES.map(type =>
      [type, 'unknown']));
    if (request.configuredType === 'Kraken') {
      const endpoint = configuredKrakenEndpoint(request.config);
      if (!endpoint) {
        errors.push(errorRecord('INVALID_CONFIGURED_ENDPOINT', 'Kraken',
          'The configured Heimdall endpoint is incomplete or invalid.'));
      } else {
        const raw = await runProbe('configured-upstream-status',
          probes.configuredUpstreamStatus, endpoint, timeoutMs, errors, null);
        if (raw !== null) {
          try { upstream = normalizeUpstream(raw); }
          catch (error) {
            errors.push(errorRecord('INVALID_PROBE_RESULT',
              'configured-upstream-status', error.message));
          }
        }
        if (isLoopback(endpoint.host)) {
          const rawService = await runProbe('service-status', probes.serviceStatus,
            {serviceId: 'kraken-suite-v2'}, timeoutMs, errors, null);
          if (rawService !== null) {
            try { serviceStates.Kraken = normalizeService(rawService); }
            catch (error) {
              errors.push(errorRecord('INVALID_PROBE_RESULT', 'service-status', error.message));
            }
          }
        }
      }
    }
    // SDRplay's service is relevant when it is configured, when its SDK/adapter
    // is present, or when a matching USB descriptor was observed. Do not let a
    // different saved receiver hide an already-running local SDRplay service.
    const rspDuoRelevant = request.configuredType === 'RspDuo' ||
      dependencies.RspDuo.state === 'installed' ||
      nativeStatus?.RspDuo?.moduleLoadable === true ||
      usb.some(device => usbMatches('RspDuo', device));
    if (rspDuoRelevant) {
      const rawService = await runProbe('service-status', probes.serviceStatus,
        {serviceId: 'sdrplay-api'}, timeoutMs, errors, null);
      if (rawService !== null) {
        try { serviceStates.RspDuo = normalizeService(rawService); }
        catch (error) {
          errors.push(errorRecord('INVALID_PROBE_RESULT', 'service-status', error.message));
        }
      }
    }

    const receivers = RECEIVER_TYPES.map(type => {
      const configured = request.configuredType === type;
      const locality = localityFor(type, configured, request.config);
      const modelMatches = usb.filter(device => usbMatches(type, device));
      const {matches, identityMatched} = configuredUsbMatches(type, request.config, modelMatches);
      const detected = type === 'Kraken' ? upstream.availability === 'available' :
        type === 'HackRF' ? matches.length >= 2 : matches.length >= 1;
      let dependency = dependencies[type];
      if (type === 'Kraken' && upstream.availability === 'available') {
        dependency =
          {state: 'installed', installed: [DEFINITIONS.Kraken.dependency],
            missing: [], unknown: []};
      } else if (type === 'Kraken' && locality === 'remote') {
        dependency =
          {state: 'unknown', installed: [], missing: [],
            unknown: [DEFINITIONS.Kraken.dependency]};
      }
      const native = nativeStatus?.[type] || null;
      // A status result has precedence. The older environment manifest remains
      // a conservative fallback while upgrading installations.
      const liveCompiled = native ? native.compiled : request.compiledLiveTypes.has(type);
      const runtimeLoadable = native ? native.moduleLoadable : null;
      // A source kit is an explicit local-build capability, not evidence that
      // an adapter is compiled, loadable, or ready for live capture.
      const localBuildable = type === 'RspDuo' && native?.localBuildable === true;
      // Successful loading resolves the adapter's actual SDK dependencies. A
      // local licensed SDRplay API need not appear in ldconfig's cache; this
      // evidence establishes software presence, never a device or running API.
      if (type !== 'Kraken' && liveCompiled && runtimeLoadable === true &&
          dependency.state !== 'installed')
        dependency = {state: 'installed', installed: [DEFINITIONS[type].dependency],
          missing: [], unknown: []};
      const possible = liveCompiled && runtimeLoadable !== false && dependency.state !== 'missing';
      const serviceId = DEFINITIONS[type].service;
      const service = serviceId ? {
        id: serviceId, required: true, state: locality === 'local' ?
          serviceStates[type] : 'unknown', ownedByVectorWarp: false,
        controlAvailable: false
      } : {id: null, required: false, state: 'unknown',
        ownedByVectorWarp: false, controlAvailable: false};
      return {
        type, label: DEFINITIONS[type].label, locality,
        capabilities: {detected, possible, configured, liveCompiled, localBuildable,
          runtimeLoadable, runtimeError: native?.error || null,
          ...(native ? {builtIn: native.builtIn} : {}),
          sourceSupported: DEFINITIONS[type].sourceSupported},
        detection: {configuredIdentityMatched: identityMatched,
          modelDetected: type === 'Kraken' ? detected : modelMatches.length > 0,
          state: detected ? 'detected' :
          (type === 'Kraken' ? (upstream.availability === 'unknown' ? 'unknown' :
            'not-detected') : (usbKnown ? 'not-detected' : 'unknown')),
        evidence: type === 'Kraken' ?
          (upstream.availability === 'available' ? ['configured-upstream-telemetry'] : []) :
          (matches.length ? [`${matches.length} matching read-only USB descriptor${matches.length === 1 ? '' : 's'}`] : [])},
        dependencies: dependency,
        managedService: service,
        upstream: type === 'Kraken' ? {authority: 'upstream', ...upstream} :
          {authority: 'direct', availability: 'not-applicable', capabilities: []},
        settings: getSettingsMapping(type)
      };
    });
    const observedAt = now();
    if (!Number.isFinite(observedAt)) throw new Error('options.now returned an invalid time.');
    return {schemaVersion: 1, observedAt, configuredType: request.configuredType,
      receivers, errors};
  }

  return {discover, plan: planReceiverSetup,
    getSettingsMapping};
}

module.exports = {RECEIVER_TYPES, SETTINGS_SYNC, createReceiverManager,
  getSettingsMapping, planReceiverSetup};
