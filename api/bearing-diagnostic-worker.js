'use strict';

const {createBearingDiagnostic} = require('./bearing-diagnostic');

process.once('message', message => {
  try {
    const value = createBearingDiagnostic(message.config).status();
    process.send?.({ok: true, value}, () => process.exit(0));
  } catch (error) {
    process.send?.({ok: false, error: error.message}, () => process.exit(1));
  }
});
