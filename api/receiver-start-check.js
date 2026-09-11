'use strict';

const {readConfig} = require('./config-store');
const {createReceiverJournal} = require('./receiver-journal');
const {createKrakenControlClient} = require('./receiver-sync');

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
    console.error(`Receiver startup blocked: ${error.message}`); process.exitCode = 1;
  });
}
module.exports = {checkReceiverStart};
