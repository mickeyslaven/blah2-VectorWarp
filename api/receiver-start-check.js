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

async function checkReceiverStart(filename, options = {}) {
  const read = options.readConfig || readConfig;
  const document = read(filename);
  if (!document.exists || document.readError || !document.validation.valid)
    throw new Error('Save valid receiver settings before starting the processor.');
  if (document.config.capture?.replay?.state === true) return {state: 'replay'};
  const journalReader = options.journal || createReceiverJournal(filename);
  const journal = journalReader.load();
  if (journal.reconciliationRequired || journal.state === 'saved-pending' ||
      (journal.configRevision && journal.configRevision !== document.revision))
    throw new Error('Receiver settings are pending or unresolved. Open Settings and Apply them before starting live processing.');
  let receipt = null;
  if (document.config.capture?.device?.type === 'Kraken') {
    if (process.env.VECTORWARP_LINUX_SERVICE === '1')
      (options.verifyBundledOwner || verifyBundledLocalOwner)(document.config);
    const client = options.client || createKrakenControlClient();
    receipt = await client.verify(document.config);
  }
  if (read(filename).revision !== document.revision ||
      JSON.stringify(journalReader.load()) !== JSON.stringify(journal))
    throw new Error('Receiver settings changed during startup verification. Apply and start again.');
  return {state: receipt ? 'upstream-verified' : 'sdk-startup-required',
    configRevision: document.revision, receipt};
}

if (require.main === module) {
  checkReceiverStart(process.argv[2]).then(result => {
    console.log(`Receiver startup: ${result.state}`);
  }).catch(error => {
    console.error(`Receiver startup blocked: ${error.message}`);
    process.exitCode = RECEIVER_START_BLOCKED_EXIT_STATUS;
  });
}
module.exports = {checkReceiverStart, verifyBundledLocalOwner, RECEIVER_START_BLOCKED_EXIT_STATUS};
