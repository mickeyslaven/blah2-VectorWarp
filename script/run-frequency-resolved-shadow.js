#!/usr/bin/env node
'use strict';

const {FrequencyResolvedShadow} = require('../api/frequency-resolved-shadow');

function argumentsFrom(values) {
  const options = {};
  for (let index = 0; index < values.length; ++index) {
    const key = values[index];
    if (!key.startsWith('--') || index + 1 >= values.length)
      throw new Error(`Invalid argument ${key}`);
    options[key.slice(2)] = values[++index];
  }
  return options;
}

async function main(values = process.argv.slice(2)) {
  const args = argumentsFrom(values);
  const configUrl = args['config-url'];
  if (!configUrl || !/^https?:\/\//.test(configUrl))
    throw new Error('--config-url must be an HTTP or HTTPS VectorWarp /api/config URL');
  const response = await fetch(configUrl, {signal: AbortSignal.timeout(5000)});
  if (!response.ok) throw new Error(`Config endpoint returned HTTP ${response.status}`);
  const config = await response.json();
  const numeric = (name, fallback) => args[name] === undefined ? fallback : Number(args[name]);
  config.process ||= {}; config.process.bearing ||= {};
  config.process.bearing.frequency_resolved_shadow = {
    enabled: true, fft_size: numeric('fft-size', 2048), subbands: numeric('subbands', 24),
    bandwidth_fraction: numeric('bandwidth-fraction', .9),
    diagonal_loading: numeric('diagonal-loading', .01),
    capture_seconds: numeric('capture-seconds', .25), timeout_ms: numeric('timeout-ms', 15000),
    interval_s: 86400, ...(args.output ? {path: args.output} : {})};
  const shadow = new FrequencyResolvedShadow(config, {initialDelayMs: 86400000});
  const status = await shadow.run();
  if (shadow.timer) clearTimeout(shadow.timer);
  if (status.error) throw new Error(status.error);
  const result = status.result;
  console.log(JSON.stringify({path: status.path, sequence: result.sequence,
    observations: result.observations, summary: result.summary,
    stability_from_previous: result.stability_from_previous}, null, 2));
  return status;
}

if (require.main === module) main().catch(error => {
  console.error(`frequency-resolved shadow failed: ${error.message}`); process.exitCode = 1;
});

module.exports = {argumentsFrom, main};
