'use strict';
const crypto = require('crypto');
const os = require('os');
const {createReceiverManager, RECEIVER_TYPES} = require('./receiver-manager');
const {createReceiverProbes} = require('./receiver-probes');
const {createReceiverHelperClient} = require('./receiver-helper-client');
const {createMacReceiverManagement} = require('./macos-receiver-management');
const {receiverSetupGuide} = require('./receiver-setup-guide');
const INTENT = 'receiver-management-v1';

function trustedOrigins(port, extra = [], networkInterfaces = os.networkInterfaces, warn = console.warn) {
  const explicit = [];
  for (const value of extra) {
    const url = new URL(value);
    if (!['http:', 'https:'].includes(url.protocol) || value !== url.origin)
      throw new Error('BLAH2_RECEIVER_ORIGINS must contain exact HTTP(S) origins.');
    explicit.push(value);
  }
  const addresses = new Set(['127.0.0.1', '::1', 'localhost']);
  let interfaces = {};
  try {
    interfaces = networkInterfaces();
  } catch (error) {
    warn(`Unable to enumerate local network interfaces; allowing loopback and explicit BLAH2_RECEIVER_ORIGINS only (${error.code || error.message}).`);
  }
  for (const group of Object.values(interfaces))
    for (const entry of group || []) if (!entry.address.includes('%')) addresses.add(entry.address);
  const origins = [...addresses].map(address => new URL(`http://${address.includes(':') ? `[${address}]` : address}:${port}`).origin);
  origins.push(...explicit);
  return new Set(origins);
}

function sameReceiverOrigin(req, allowed = new Set(['http://127.0.0.1:3000'])) {
  try {
    const target = `${req.protocol || 'http'}://${req.get('Host')}`;
    if (!allowed.has(target)) return false;
    const origin = req.get('Origin');
    if (origin) return allowed.has(origin) && new URL(origin).origin === origin;
    // Ordinary HTTP LAN addresses do not receive Fetch Metadata in browsers.
    // A trusted exact Referer suffices for these read-only requests; reject a
    // conflicting Fetch Metadata header if supplied. Every mutation needs Origin.
    return req.method === 'GET' && (!req.get('Sec-Fetch-Site') || req.get('Sec-Fetch-Site') === 'same-origin') &&
      allowed.has(new URL(req.get('Referer')).origin);
  } catch (_) { return false; }
}

function installReceiverRoutes(app, options) {
  const {readDocument, preview = false, compiledLiveTypes = null} = options;
  const platform = options.platform || process.platform;
  const environment = options.environment || process.env;
  const standaloneDistribution = platform === 'darwin' && environment.VECTORWARP_MACOS_DISTRIBUTION === 'standalone';
  const createProbes = options.createProbes || createReceiverProbes;
  const createManager = options.createManager || createReceiverManager;
  const allowed = options.allowedOrigins || trustedOrigins(options.port || 3000, options.extraOrigins || [],
    options.networkInterfaces || os.networkInterfaces, options.warn || console.warn);
  // macOS has no privileged receiver broker. Keep its paths unreachable even
  // when a caller supplies a Linux helper test double.
  const helper = platform === 'darwin' ? null : (options.helper || createReceiverHelperClient());
  const macManagement = platform === 'darwin' ? (options.macManagement || createMacReceiverManagement({...options.macManagementOptions, environment})) : null;
  const now = options.now || Date.now;
  const helperExecutable = options.helperExecutable || '/opt/vectorwarp/libexec/vectorwarp-receiver-helper';
  if (!/^\/[A-Za-z0-9_./+-]+$/.test(helperExecutable))
    throw new Error('The receiver helper executable must be an installed absolute path.');
  const grants = new Map();
  let cached, running = false;
  function guard(req, res, mutation = false) {
    const trusted = sameReceiverOrigin(req, allowed);
    res.removeHeader('Access-Control-Allow-Origin');
    if (trusted && req.get('Origin')) {
      res.set('Access-Control-Allow-Origin', req.get('Origin'));
      res.vary?.('Origin');
    }
    res.set('Cache-Control', 'no-store');
    if (!trusted || (mutation &&
        (req.get('X-VectorWarp-Intent') !== INTENT ||
         !/^application\/json(?:\s*;|$)/i.test(req.get('Content-Type') || '')))) {
      res.status(403).json({ok: false, code: 'RECEIVER_ORIGIN_REQUIRED',
        errors: ['Open Receiver setup from the trusted VectorWarp address and review the action there.']});
      return false;
    }
    return true;
  }
  const validBody = (body, keys) => body && typeof body === 'object' && !Array.isArray(body) &&
    Object.keys(body).every(key => keys.includes(key));
  function failure(res, error) {
    return res.status(error.status || 503).json({ok: false, code: error.code || 'RECEIVER_CHECK_FAILED',
      errors: [error.message || 'Receiver checks failed. Recheck the installed software.']});
  }
  async function management() {
    if (preview) return {available: false, code: 'PREVIEW', actions: [], message: 'Host management is disabled in UI previews.'};
    if (platform === 'darwin') return macManagement.discover();
    try {
      const result = await helper({verb: 'discover'});
      return {...result, available: result.ok === true};
    } catch (error) { return {available: false, actions: [], code: error.code, message: error.message}; }
  }
  async function snapshot(fresh = false) {
    const document = readDocument();
    if (!fresh && cached && cached.revision === document.revision && now() - cached.at < 5000) return cached.promise;
    const manager = createManager({timeoutMs: 1800, standaloneDistribution,
      probes: preview ? {} : createProbes(document.config, {platform, env: environment})});
    const promise = Promise.all([manager.discover({config: document.config, compiledLiveTypes: compiledLiveTypes || []}), management()])
      .then(([discovery, managed]) => {
        const remoteSuite = discovery.receivers?.some(receiver => receiver.type === 'Kraken' &&
          receiver.capabilities.configured && receiver.locality === 'remote');
        if (remoteSuite && Array.isArray(managed.actions)) managed = {...managed,
          actions: managed.actions.map(action => action.receiverType === 'Kraken' && action.kind === 'start-service' ?
            {...action, available: false, code: 'REMOTE_ENDPOINT',
              message: 'The saved Suite endpoint is remote; local receiver service control does not apply.'} : action)};
        return {...discovery,
        receivers: discovery.receivers.map(receiver => ({...receiver,
          setupGuide: receiverSetupGuide(receiver, helperExecutable, {platform})})),
        configRevision: document.revision,
        setupRequired: document.setupRequired === true, buildCapabilitiesKnown: compiledLiveTypes !== null,
        preview, managementAvailable: managed.available, management: managed,
        processor: options.processorStatus?.() || null};
      });
    cached = {revision: document.revision, at: now(), promise};
    promise.catch(() => { if (cached?.promise === promise) cached = null; });
    return promise;
  }
  app.get('/api/receivers', async (req, res) => {
    if (!guard(req, res)) return;
    try { res.json(await snapshot()); } catch (error) { failure(res, error); }
  });
  app.post('/api/receivers/discover', async (req, res) => {
    if (!guard(req, res, true)) return;
    if (!validBody(req.body, [])) return res.status(422).json({ok: false, errors: ['Discovery takes no receiver or endpoint overrides.']});
    try { res.json(await snapshot(true)); } catch (error) { failure(res, error); }
  });
  app.post('/api/receivers/plan', async (req, res) => {
    if (!guard(req, res, true)) return;
    if (!validBody(req.body, ['receiverType', 'actionId']) || !RECEIVER_TYPES.includes(req.body.receiverType) ||
        (req.body.actionId !== undefined && !/^[a-z][a-z0-9-]{2,63}$/.test(req.body.actionId)))
      return res.status(422).json({ok: false, errors: ['Select a supported receiver and reviewed action.']});
    try {
      const discovery = await snapshot(true);
      if (readDocument().revision !== discovery.configRevision)
        return res.status(409).json({ok: false, errors: ['Settings changed during receiver checks. Reload and try again.']});
      if (!req.body.actionId) return res.json({...createManager().plan({receiverType: req.body.receiverType}, discovery),
        configRevision: discovery.configRevision, managementAvailable: discovery.managementAvailable,
        reviewedActions: (discovery.management.actions || []).filter(action => action.receiverType === req.body.receiverType)});
      const action = discovery.management.actions?.find(item => item.id === req.body.actionId && item.receiverType === req.body.receiverType);
      if (preview || !action?.available)
        return res.status(409).json({ok: false, code: 'ACTION_NOT_REVIEWED', errors: ['This installation has no available reviewed action for this receiver.']});
      const result = platform === 'darwin' ? macManagement.plan(action.id) :
        await helper({verb: 'plan', actionId: action.id, configRevision: discovery.configRevision});
      if (!result) return res.status(409).json({ok: false, code: 'ACTION_NOT_REVIEWED', errors: ['This installation has no available reviewed action for this receiver.']});
      if (!result.ok) return res.status(409).json({...result, errors: [result.message]});
      if (result.status === 'not-required') return res.json({...result, configRevision: discovery.configRevision});
      if (platform !== 'darwin' && !/^[a-f0-9]{64}$/.test(result.planId)) throw new Error('The receiver helper returned an invalid plan ID.');
      for (const [id, grant] of grants) if (grant.expiresAt <= now()) grants.delete(id);
      if (grants.size >= 64) return res.status(429).json({ok: false, errors: ['Too many plans are pending; wait for them to expire.']});
      const nonce = crypto.randomBytes(32).toString('hex');
      const grant = {planId: result.planId, actionId: action.id, macos: platform === 'darwin', configRevision: discovery.configRevision,
        origin: req.get('Origin'), expiresAt: now() + Math.min(result.lifetimeSeconds || 300, 300) * 1000};
      grants.set(nonce, grant);
      res.json({...result, nonce, configRevision: grant.configRevision, expiresAt: grant.expiresAt,
        ...(platform === 'darwin' ? {} : {authorizationCommand: `sudo ${helperExecutable} authorize ${result.planId}`})});
    } catch (error) { failure(res, error); }
  });
  app.post('/api/receivers/execute', async (req, res) => {
    if (!guard(req, res, true)) return;
    if (!validBody(req.body, ['nonce', 'configRevision']) || !/^[a-f0-9]{64}$/.test(req.body.nonce || ''))
      return res.status(422).json({ok: false, errors: ['A one-use receiver plan is required.']});
    const grant = grants.get(req.body.nonce);
    if (preview || !grant || grant.expiresAt <= now() || grant.origin !== req.get('Origin'))
      return res.status(409).json({ok: false, code: 'PLAN_EXPIRED', errors: ['The receiver plan expired or was already used. Review a fresh plan.']});
    if (running || options.transactionBusy?.())
      return res.status(409).json({ok: false, code: 'MANAGEMENT_BUSY', errors: ['Another receiver/settings transaction is running. Wait for its result.']});
    if (req.body.configRevision !== grant.configRevision || readDocument().revision !== grant.configRevision) {
      grants.delete(req.body.nonce);
      return res.status(409).json({ok: false, code: 'CONFIG_CHANGED', errors: ['Saved settings changed. Review a fresh receiver plan.']});
    }
    // Root broker consumes its own grant. Keep a browser nonce only for the
    // harmless response that local authorization has not happened yet.
    grants.delete(req.body.nonce);
    running = true;
    try {
      const result = grant.macos ? await macManagement.execute(grant.actionId) :
        await helper({verb: 'execute', planId: grant.planId, configRevision: grant.configRevision});
      if (result.code === 'LOCAL_AUTHORIZATION_REQUIRED') grants.set(req.body.nonce, grant);
      cached = null;
      res.status(result.ok ? 200 : 409).json({...result, ...(result.ok ? {} : {errors: [result.message]})});
    } catch (error) { failure(res, error); }
    finally { running = false; }
  });
  return {busy: () => running};
}
module.exports = {installReceiverRoutes, sameReceiverOrigin, trustedOrigins};
