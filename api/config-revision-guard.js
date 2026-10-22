'use strict';

function evaluateConfigRevision(document, journal) {
  if (!document?.exists || document.readError || document.validation?.valid !== true)
    return {quiesce: true, reason: 'config-invalid'};
  if (!journal?.exists)
    return {quiesce: true, reason: 'receiver-journal-missing'};
  if (journal.configRevision !== document.revision)
    return {quiesce: true, reason: 'config-revision-unsettled'};
  if (journal.reconciliationRequired === true || journal.applicationVerified !== true ||
      !['not-required', 'already-matched', 'synchronized'].includes(journal.state))
    return {quiesce: true, reason: 'receiver-reconciliation-required'};
  return {quiesce: false, reason: 'revision-settled'};
}

module.exports = {evaluateConfigRevision};
