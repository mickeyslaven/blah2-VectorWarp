'use strict';

const {readConfig} = require('./config-store');
const {createReceiverJournal} = require('./receiver-journal');
const {createKrakenControlClient} = require('./receiver-sync');
const fs = require('fs');
const {execFileSync} = require('child_process');

function verifyBundledLocalOwner(config, {read = fs.readFileSync, execute = execFileSync} = {}) {
  const endpoint = config.capture?.device?.heimdall;
  if (endpoint?.host !== '127.0.0.1') return;
  const unit = 'vectorwarp-heimdall.service';
  execute('/usr/bin/systemctl', ['is-active', '--quiet', unit], {timeout: 3000});
  const pid = Number(execute('/usr/bin/systemctl', ['show', '--property=MainPID', '--value', unit],
    {encoding: 'utf8', timeout: 3000}));
  const ready = JSON.parse(read('/run/vectorwarp-heimdall/ready.json', 'utf8'));
  if (!Number.isSafeInteger(pid) || pid <= 0 || ready.pid !== pid || ready.status !== 'ready' ||
      ready.bindHost !== '127.0.0.1' || ready.dataPort !== endpoint.port ||
      ready.controlPort !== (endpoint.control_port ?? 8092))
    throw new Error('Local Kraken is not owned by this VectorWarp bundled receiver service.');
}

// EX_CONFIG: startup is intentionally blocked until receiver state/configuration
// changes.  The systemd unit treats this differently from a processor crash.
const RECEIVER_START_BLOCKED_EXIT_STATUS = 78;
const TRANSIENT_LOCAL_KRAKEN_START_CODES = new Set([
  'KRAKEN_CONNECTION_FAILED',
  'KRAKEN_CONNECTION_CLOSED',
  'KRAKEN_STATUS_TIMEOUT',
  'KRAKEN_CALIBRATION_UNVERIFIED',
  'KRAKEN_BUSY'
]);

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

async function checkReceiverStart(filename, options = {}) {
  const read = options.readConfig || readConfig;
  const document = read(filename);
  if (!document.exists || document.readError || !document.validation.valid)
    throw new Error('Save valid receiver settings before starting the processor. ' +
      (document.validation?.errors || []).join('; '));
  if (document.config.capture?.replay?.state === true) return {state: 'replay'};
  const journalReader = options.journal || createReceiverJournal(filename);
  const journal = journalReader.load();
  const revisionChanged = Boolean(journal.configRevision &&
    journal.configRevision !== document.revision);
  if (journal.reconciliationRequired || journal.state === 'saved-pending' ||
      (revisionChanged && document.config.capture?.device?.type !== 'Kraken'))
    throw new Error('Receiver settings are pending or unresolved. Open Settings and Apply them before starting live processing.');
  let receipt = null;
  if (document.config.capture?.device?.type === 'Kraken') {
    if (process.env.VECTORWARP_LINUX_SERVICE === '1')
      (options.verifyBundledOwner || verifyBundledLocalOwner)(document.config);
    const client = options.client || createKrakenControlClient();
    receipt = await client.verify(document.config);
    // A safe schema/default migration can change the whole-file revision while
    // leaving every receiver setting untouched. Kraken has an authoritative
    // upstream readback, so its read-only verification may clear this start
    // check. The processor service intentionally cannot write /etc; the API's
    // normal receiver transaction remains the durable journal writer.
  }
  const verified = read(filename);
  if (verified.revision !== document.revision || !verified.validation.valid ||
      verified.radarPlan?.requirement_sha256 !== document.radarPlan?.requirement_sha256 ||
      (!revisionChanged && JSON.stringify(journalReader.load()) !== JSON.stringify(journal)))
    throw new Error('Receiver settings changed during startup verification. Apply and start again.');
  return {state: receipt ? 'upstream-verified' : 'sdk-startup-required',
    configRevision: document.revision, receipt};
}

async function checkReceiverStartWithLocalReadiness(filename, options = {}) {
  const attempts = options.attempts ?? 30;
  const wait = options.wait || delay;
  for (let attempt = 1; ; attempt += 1) {
    try {
      return await (options.check || checkReceiverStart)(filename, options.checkOptions);
    } catch (error) {
      const transient = process.env.VECTORWARP_LINUX_SERVICE === '1' &&
        TRANSIENT_LOCAL_KRAKEN_START_CODES.has(error?.code);
      if (!transient || attempt >= attempts) throw error;
      await wait(1000);
    }
  }
}

if (require.main === module) {
  checkReceiverStartWithLocalReadiness(process.argv[2]).then(result => {
    console.log(`Receiver startup: ${result.state}`);
  }).catch(error => {
    console.error(`Receiver startup blocked: ${error.message}`);
    process.exitCode = RECEIVER_START_BLOCKED_EXIT_STATUS;
  });
}
module.exports = {checkReceiverStart, checkReceiverStartWithLocalReadiness,
  verifyBundledLocalOwner, RECEIVER_START_BLOCKED_EXIT_STATUS};
