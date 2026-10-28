(function (root) {
  'use strict';
  const c = 299792458;
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const escape = value => String(value ?? '').replace(/[&<>"']/g, character =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;', "'":'&#39;'}[character]));
  const channels = config => config.capture?.device?.channel_count ||
    config.capture?.device?.gain?.length || config.capture?.device?.antenna?.length || 2;
  const formatBytes = value => {
    const units = ['B', 'KB', 'MB', 'GB', 'TB']; let amount = value, unit = 0;
    while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit++; }
    return `${amount.toFixed(unit < 2 ? 0 : 1)} ${units[unit]}`;
  };
  const configuredSite = site => finite(site?.latitude) && finite(site?.longitude) &&
    (Math.abs(site.latitude) > 1e-8 || Math.abs(site.longitude) > 1e-8) &&
    !/^Set (receiver|transmitter) site$/.test(site.name || '');
  function recordingEstimate(config) {
    const fs = Number(config.capture?.fs) || 0, count = channels(config);
    const kraken = config.capture?.device?.type === 'Kraken';
    const manualPerSecond = fs * count * 8;
    const clipPerSecond = fs * count * (kraken ? 2 : 8);
    const clipSeconds = Number(config.capture?.aircraft_clips?.pre_seconds || 0) +
      Number(config.capture?.aircraft_clips?.post_seconds || 0);
    return {manualPerSecond, clipPerSecond, clipSeconds,
      manualHour: manualPerSecond * 3600, clip: clipPerSecond * clipSeconds,
      clipLabel: kraken ? 'Compact native 8-bit Kraken IQ' : 'Portable float32 IQ'};
  }
  function searchWindow(config, rangeKm, speedMps) {
    const sampleRate = Number(config.capture?.fs), frequency = Number(config.capture?.fc);
    if (!(sampleRate > 0) || !(frequency > 0) || !(rangeKm > 0) || !(speedMps > 0)) return null;
    const kmPerBin = c / sampleRate / 1000;
    return {delayMax: Math.ceil(2 * rangeKm / kmPerBin),
      dopplerMax: Math.ceil((2 * speedMps * frequency / c) / 10) * 10,
      kmPerBin};
  }
  const card = (id, title, state, body, action = '') => `<article class="setup-card" id="${id}"><header><strong>${escape(title)}</strong><span>${escape(state)}</span></header><p>${escape(body)}</p>${action}</article>`;
  async function enrich(target) {
    try {
      const [statusResponse, calibrationResponse, surveyResponse] = await Promise.all([
        fetchStatusResource(liveApiUrl('/api/system/status'), {cache: 'no-store'}),
        fetchStatusResource(liveApiUrl('/api/array-calibration/status'), {cache: 'no-store'}),
        fetchStatusResource(liveApiUrl('/api/illuminator-survey'), {cache: 'no-store'})
      ]);
      const status = statusResponse.ok ? await statusResponse.json() : null;
      const acceleration = target.querySelector('#setup-performance');
      if (acceleration && status) {
        const active = status.acceleration?.active === 'vulkan';
        acceleration.querySelector('header span').textContent = active ? 'GPU active' : 'CPU active';
        acceleration.querySelector('p').textContent = active ?
          `${status.acceleration.device || 'Qualified GPU'} is processing delay–Doppler.` :
          `GPU is ${status.acceleration?.state === 'checking' ? 'being checked' : 'not active'}; VectorWarp is using CPU processing.`;
      }
      const health = target.querySelector('#setup-health');
      if (health && status) {
        const errors = Array.isArray(status.errors) ? status.errors.filter(Boolean) : [];
        health.querySelector('header span').textContent = status.radar === 'receiving' ? 'Receiving' : 'Needs review';
        health.querySelector('p').textContent = errors.length ? errors.join(' ') :
          status.radar === 'receiving' ? 'Receiver and processing frames are current.' :
            (status.message || 'Live radar data is not current.');
      }
      const calibration = calibrationResponse.ok ? await calibrationResponse.json() : null;
      const calibrationCard = target.querySelector('#setup-calibration');
      if (calibrationCard && calibration) {
        const corpus = calibration.corpus || {};
        calibrationCard.querySelector('header span').textContent = corpus.enabled ?
          `${corpus.accepted_samples_this_process || 0} accepted now` : 'Collection off';
        calibrationCard.querySelector('p').textContent = corpus.error ||
          (corpus.enabled ? 'ADS-B-labelled array responses are being collected. Bearing and detection models qualify separately.' :
            'Array calibration collection is not enabled for this receiver configuration.');
      }
      const survey = surveyResponse.ok ? await surveyResponse.json() : null;
      const surveyCard = target.querySelector('#setup-illuminator');
      if (surveyCard && survey) {
        const plans = Object.keys(survey.band_plans || {}).length;
        surveyCard.querySelector('header span').textContent = survey.capabilities?.rf_prescreen ? 'Receiver supported' : 'Limited';
        surveyCard.querySelector('p').textContent = survey.capabilities?.rf_prescreen ?
          `${plans} broadcast band plans are available. Candidate measurements can be ranked by headroom, waveform quality, cancellation and held-out radar performance.` :
          (survey.capabilities?.limitation || 'This receiver cannot run the coherent RF prescreen.');
      }
    } catch (_) { /* Normal settings remain available if an assistant status fails. */ }
  }
  function render(target, api) {
    if (!target) return;
    const config = api.getConfig(), estimate = recordingEstimate(config);
    const rx = configuredSite(config.location?.rx), tx = configuredSite(config.location?.tx);
    const receiver = config.capture?.device?.type || 'Unconfigured';
    const measured = config.capture?.device?.heimdall?.gain_control === 'measured';
    target.innerHTML = `<section class="setup-assistant"><div class="setup-assistant-heading"><div><h2>Setup assistant</h2><p>Safe essentials first. Advanced mode reveals every DSP and service setting without changing saved values.</p></div><div class="settings-mode" role="group" aria-label="Settings detail"><button type="button" data-mode="simple" class="${api.mode() === 'simple' ? 'active' : ''}">Simple</button><button type="button" data-mode="advanced" class="${api.mode() === 'advanced' ? 'active' : ''}">Advanced</button></div></div><div class="setup-grid">
      ${card('setup-receiver', 'Receiver', receiver === 'Unconfigured' ? 'Not selected' : receiver, receiver === 'Unconfigured' ? 'Choose and check a connected receiver before starting.' : 'Receiver dependencies and readback are checked before settings are applied.', '<button type="button" data-tab="capture">Receiver settings</button>')}
      ${card('setup-sites', 'Receiver and transmitter sites', rx && tx ? 'Configured' : 'Incomplete', rx && tx ? 'Both physical sites have coordinates and altitude.' : 'Set both sites so delay, range, geometry and ADS-B comparisons have physical meaning.', '<button type="button" data-tab="location">Site settings</button>')}
      ${card('setup-gain', 'Safe gain', receiver === 'Kraken' ? (measured ? 'Automatic check' : 'Fixed') : 'Receiver controlled', receiver === 'Kraken' ? (measured ? 'Apply measures every active channel, rejects unsafe headroom and records the accepted gain.' : 'Fixed gain is useful for controlled comparisons; automatic headroom checking is available.') : 'Gain controls and overload feedback follow the selected receiver.', receiver === 'Kraken' && !measured ? '<button type="button" data-auto-gain>Use automatic safe gain</button>' : '<button type="button" data-tab="capture">Review gain</button>')}
      ${card('setup-illuminator', 'Illuminator survey', 'Reading capabilities', 'Checking the selected receiver and broadcast band plans.', '<button type="button" data-tab="capture">Frequency settings</button>')}
      ${card('setup-window', 'Search window', 'Automatic calculator', 'Choose the farthest receiver range and fastest target; VectorWarp calculates conservative delay and Doppler limits.', '<div class="setup-window-inputs"><label>Range (km)<input type="number" min="1" max="1000" value="50" data-range></label><label>Speed (m/s)<input type="number" min="10" max="3000" value="300" data-speed></label><button type="button" data-window>Calculate & apply</button></div><small data-window-result></small>')}
      ${card('setup-performance', 'Processing hardware', 'Reading status', 'Checking whether qualified GPU processing is active.', '<button type="button" data-tab="process">Performance settings</button>')}
      ${card('setup-recording', 'Recording storage', estimate.clipLabel, `Manual recording uses ${formatBytes(estimate.manualHour)} per hour. One ${estimate.clipSeconds.toFixed(1)} second aircraft clip is approximately ${formatBytes(estimate.clip)}.`, '<button type="button" data-tab="save">Recording settings</button>')}
      ${card('setup-calibration', 'Array calibration', 'Reading status', 'Checking installed-array evidence collection and qualification.', '<a class="button-link" href="/display/calibration">Open calibration assistant</a>')}
      ${card('setup-health', 'System health', 'Reading status', 'Checking current receiver and processing state.', '<a class="button-link" href="/display/health">Open system health</a>')}
    </div></section>`;
    target.querySelectorAll('[data-mode]').forEach(button => button.addEventListener('click', () => api.setMode(button.dataset.mode)));
    target.querySelectorAll('[data-tab]').forEach(button => button.addEventListener('click', () => api.openTab(button.dataset.tab)));
    target.querySelector('[data-auto-gain]')?.addEventListener('click', () => {
      api.set(['capture', 'device', 'heimdall', 'gain_control'], 'measured'); api.refresh();
    });
    target.querySelector('[data-window]')?.addEventListener('click', () => {
      const range = Number(target.querySelector('[data-range]').value);
      const speed = Number(target.querySelector('[data-speed]').value);
      const result = searchWindow(config, range, speed);
      const output = target.querySelector('[data-window-result]');
      if (!result) { output.textContent = 'Enter a valid range and speed after selecting a receiver frequency and sample rate.'; return; }
      api.set(['process', 'ambiguity', 'delayMax'], result.delayMax);
      api.set(['process', 'ambiguity', 'dopplerMin'], -result.dopplerMax);
      api.set(['process', 'ambiguity', 'dopplerMax'], result.dopplerMax);
      if (config.process?.clutter?.followAmbiguity) {
        api.set(['process', 'clutter', 'delayMin'], config.process.ambiguity.delayMin);
        api.set(['process', 'clutter', 'delayMax'], result.delayMax);
      }
      output.textContent = `Applied ${result.delayMax} delay bins (≈${(result.delayMax * result.kmPerBin).toFixed(1)} km excess path) and ±${result.dopplerMax} Hz. Save & Restart when ready.`;
      api.refreshEditor();
    });
    void enrich(target);
  }
  root.VectorWarpSetupAssistant = {render, recordingEstimate, searchWindow};
  if (typeof module !== 'undefined' && module.exports)
    module.exports = {render, recordingEstimate, searchWindow};
})(typeof window !== 'undefined' ? window : globalThis);
