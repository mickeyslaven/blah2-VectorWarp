'use strict';
const assert = require('assert');
const {evaluateConfigRevision} = require('./config-revision-guard');

const document = {exists: true, revision: 'new', validation: {valid: true}};
assert.deepEqual(evaluateConfigRevision(document, {exists: true, configRevision: 'old',
  state: 'synchronized', applicationVerified: true, reconciliationRequired: false}),
{quiesce: true, reason: 'config-revision-unsettled'});
assert.deepEqual(evaluateConfigRevision(document, {exists: true, configRevision: 'new',
  state: 'saved-pending', applicationVerified: false, reconciliationRequired: true}),
{quiesce: true, reason: 'receiver-reconciliation-required'});
assert.deepEqual(evaluateConfigRevision(document, {exists: true, configRevision: 'new',
  state: 'already-matched', applicationVerified: true, reconciliationRequired: false}),
{quiesce: false, reason: 'revision-settled'});
assert.deepEqual(evaluateConfigRevision({exists: true, readError: 'bad', validation: {valid: false}}, {}),
{quiesce: true, reason: 'config-invalid'});
console.log('PASS direct-write revision guard fails closed until receiver reconciliation');
