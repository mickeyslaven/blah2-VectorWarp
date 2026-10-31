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
  const gpuActive = value => typeof value?.active === 'string' &&
    value.active.startsWith('vulkan') && value.state === 'ready';
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
  let wizardOpen = false;
  let wizardStep = 0;
  const wizardVisited = new Set();
  let wizardBusy = false;
  let wizardError = '';
  let wizardNotice = '';
  let wizardAutomatic = null;

  const recommendedAutomatic = () => ({
    detection: true,
    tracker: true,
    adsb: true,
    clips: true
  });

  function receiverHardwareReady(device = {}) {
    if (device.type === 'HackRF')
      return Array.isArray(device.serial) && device.serial.length >= 2 &&
        device.serial.slice(0, 2).every(value => /^[0-9a-f]{1,32}$/i.test(String(value || '')));
    if (device.type === 'Kraken')
      return Boolean(device.heimdall?.host) && Number(device.heimdall?.port) > 0 &&
        Number(device.heimdall?.control_port) > 0;
    if (device.type === 'Usrp') return Boolean(device.address && device.subdev);
    return device.type === 'RspDuo' || Boolean(device.type && device.type !== 'Unconfigured');
  }

  function wizardChecks(config, profiles = []) {
    const clips = config.capture?.aircraft_clips;
    const selected = profiles.find(profile => profile.type === config.capture?.device?.type);
    const receiverConfigured = Boolean(config.capture?.device?.type &&
      config.capture.device.type !== 'Unconfigured' && receiverHardwareReady(config.capture.device));
    return [
      {title: 'Receiver', complete: receiverConfigured && selected?.liveAvailable !== false,
        detail: selected?.liveAvailable === false ? 'Receiver software is not ready' : null},
      {title: 'Tuning', complete: Number(config.capture?.fc) > 0 && Number(config.capture?.fs) > 0},
      {title: 'Sites', complete: configuredSite(config.location?.rx) && configuredSite(config.location?.tx)},
      {title: 'Radar', complete: Number(config.process?.data?.cpi) > 0 &&
        typeof config.process?.detection?.enable === 'boolean' &&
        typeof config.process?.tracker?.enable === 'boolean'},
      {title: 'Data', complete: wizardVisited.has(4) ||
        (typeof config.truth?.adsb?.enabled === 'boolean' && (!clips || typeof clips.enabled === 'boolean'))},
      {title: 'Review', complete: false}
    ];
  }

  const valueAttr = value => escape(value === null || value === undefined ? '' : value);
  const checked = value => value === true ? ' checked' : '';

  function wizardPanel(config, index, profiles) {
    const estimate = recordingEstimate(config);
    if (index === 0) {
      const selected = profiles.find(profile => profile.type === config.capture?.device?.type);
      const hardwareReady = receiverHardwareReady(config.capture?.device);
      const readiness = selected?.liveAvailable === false ?
        '<p class="setup-wizard-readiness warning"><strong>Receiver software required.</strong> Install or build the selected receiver adapter before VectorWarp can start live capture.</p><button type="button" class="button-secondary" data-wizard-receiver-software>Open receiver software setup</button>' :
        hardwareReady ?
          `<p class="setup-wizard-readiness good"><strong>${selected ? 'Receiver ready.' : 'Choose a receiver.'}</strong></p>` :
          '<p class="setup-wizard-readiness warning"><strong>Hardware identity required.</strong> Complete the receiver fields below before continuing.</p>';
      const device = config.capture?.device || {};
      return `<h2>Choose the receiver</h2><label class="setup-wizard-field">Receiver<select data-wizard-receiver>${profiles.map(profile => `<option value="${escape(profile.type)}"${profile.type === config.capture?.device?.type ? ' selected' : ''}>${escape(profile.label || profile.type)}</option>`).join('')}</select></label>${readiness}<p class="setup-wizard-hint">Detected receivers are selected automatically.</p>`;
    }
    if (index === 1) return `<h2>Set frequency and sample rate</h2><div class="setup-wizard-fields"><label class="setup-wizard-field">Center frequency (MHz)<input data-wizard-fc type="number" min="0.001" step="0.001" value="${valueAttr(Number(config.capture?.fc || 0) / 1e6)}"></label><label class="setup-wizard-field">Sample rate (MS/s)<input data-wizard-fs type="number" min="0.001" step="0.001" value="${valueAttr(Number(config.capture?.fs || 0) / 1e6)}"></label></div>`;
    if (index === 2) {
      const site = (role, title) => { const item = config.location?.[role] || {}; return `<fieldset class="setup-wizard-site"><legend>${title}</legend><label>Name<input data-site="${role}.name" value="${valueAttr(item.name)}"></label><label>Latitude<input data-site="${role}.latitude" type="number" min="-90" max="90" step="any" value="${valueAttr(item.latitude)}"></label><label>Longitude<input data-site="${role}.longitude" type="number" min="-180" max="180" step="any" value="${valueAttr(item.longitude)}"></label><label>Altitude (m)<input data-site="${role}.altitude" type="number" step="any" value="${valueAttr(item.altitude)}"></label></fieldset>`; };
      return `<h2>Locate the receiver and illuminator</h2><div class="setup-wizard-sites">${site('rx','Receiver site')}${site('tx','Illuminator site')}</div>`;
    }
    if (index === 3) return `<h2>Choose the radar search</h2><p>Range and speed set the delay and Doppler limits.</p><div class="setup-wizard-fields"><label class="setup-wizard-field">CPI (seconds)<input data-wizard-cpi type="number" min="0.01" step="0.01" value="${valueAttr(config.process?.data?.cpi)}"></label><label class="setup-wizard-field">Maximum range (km)<input data-wizard-range type="number" min="1" max="1000" value="50"></label><label class="setup-wizard-field">Maximum speed (m/s)<input data-wizard-speed type="number" min="10" max="3000" value="300"></label></div>`;
    if (index === 4) return `<h2>Review storage</h2><div class="setup-wizard-summary"><strong>Storage estimate</strong><span>${estimate.clipLabel}</span><span>Manual IQ: ${formatBytes(estimate.manualHour)} per hour</span><span>Aircraft clip: about ${formatBytes(estimate.clip)} each</span></div>`;
    const checks = wizardChecks(config, profiles).slice(0, 5);
    const automatic = wizardAutomatic || recommendedAutomatic();
    const option = (key, label) => `<label><input data-wizard-automatic="${key}" type="checkbox"${checked(automatic[key])}><span>${label}</span><strong class="setup-recommended">RECOMMENDED</strong></label>`;
    return `<h2>Review setup</h2><p>Finish applies these settings and starts the radar.</p><div class="setup-wizard-review">${checks.map(item => `<div class="${item.complete ? 'complete' : 'incomplete'}"><span>${item.complete ? '✓' : '!'}</span><strong>${escape(item.title)}</strong><small>${item.complete ? 'Ready' : 'Needs attention'}</small></div>`).join('')}</div><details class="setup-wizard-automatic"><summary>Automatic features <span class="setup-recommended">RECOMMENDED</span></summary><div class="setup-wizard-options">${option('detection', 'Detect radar returns')}${option('tracker', 'Build persistent tracks')}${option('adsb', 'Compare ADS-B aircraft')}${config.capture?.aircraft_clips ? option('clips', 'Record eligible aircraft clips') : ''}</div></details><p class="setup-wizard-hint">Receiver: <strong>${escape(config.capture?.device?.type || 'Unconfigured')}</strong> · ${(Number(config.capture?.fc || 0) / 1e6).toFixed(3)} MHz · ${(Number(config.capture?.fs || 0) / 1e6).toFixed(3)} MS/s</p>`;
  }

  function mountWizard(target, api) {
    if (!wizardOpen) return;
    const config = api.getConfig();
    const profiles = api.receiverProfiles();
    const checks = wizardChecks(config, profiles);
    wizardStep = Math.max(0, Math.min(checks.length - 1, wizardStep));
    const locked = index => index > 0 && checks.slice(0, index).some(item => !item.complete);
    const dialog = target.ownerDocument.createElement('dialog');
    dialog.className = 'setup-wizard';
    dialog.setAttribute('aria-labelledby', 'setup-wizard-title');
    dialog.innerHTML = `<div class="setup-wizard-shell"><aside><div><strong id="setup-wizard-title">VectorWarp setup</strong><small>Step ${wizardStep + 1} of ${checks.length}</small></div><nav aria-label="Setup steps">${checks.map((item, index) => `<button type="button" data-wizard-step="${index}" class="${index === wizardStep ? 'active' : ''} ${item.complete ? 'complete' : ''}"${index === wizardStep ? ' aria-current="step"' : ''}${locked(index) ? ' disabled' : ''}><span>${item.complete ? '✓' : index + 1}</span>${escape(item.title)}</button>`).join('')}</nav></aside><section class="setup-wizard-content">${wizardPanel(config, wizardStep, profiles)}<p class="setup-wizard-notice" role="status"${wizardNotice ? '' : ' hidden'}>${escape(wizardNotice)}</p><p class="setup-wizard-error" role="alert"${wizardError ? '' : ' hidden'}>${escape(wizardError)}</p><footer><button type="button" class="button-secondary" data-wizard-close>Cancel</button><span></span>${wizardStep ? '<button type="button" class="button-secondary" data-wizard-back>Back</button>' : ''}<button type="button" class="button-primary" data-wizard-next>${wizardStep === checks.length - 1 ? 'Finish & Start radar' : 'Save step & Continue'}</button></footer></section></div>`;
    target.appendChild(dialog);
    const close = () => { wizardOpen = false; wizardAutomatic = null;
      if (typeof dialog.close === 'function') dialog.close(); dialog.remove(); };
    dialog.querySelector('[data-wizard-close]').addEventListener('click', close);
    dialog.querySelector('[data-wizard-receiver]')?.addEventListener('change', event => {
      wizardError = '';
      api.switchReceiver(event.currentTarget.value);
    });
    dialog.querySelector('[data-wizard-receiver-software]')?.addEventListener('click', () => {
      close(); api.openTab('capture');
    });
    dialog.addEventListener('cancel', event => { event.preventDefault(); close(); });
    dialog.querySelectorAll('[data-wizard-step]').forEach(button => button.addEventListener('click', () => {
      wizardStep = Number(button.dataset.wizardStep); render(target, api);
    }));
    dialog.querySelectorAll('[data-wizard-automatic]').forEach(input =>
      input.addEventListener('change', () => { wizardAutomatic[input.dataset.wizardAutomatic] = input.checked; }));
    dialog.querySelector('[data-wizard-back]')?.addEventListener('click', () => {
      wizardStep -= 1; render(target, api);
    });
    dialog.querySelector('[data-wizard-next]').addEventListener('click', async event => {
      if (wizardBusy) return;
      wizardBusy = true; wizardError = '';
      const next = event.currentTarget;
      next.disabled = true; next.textContent = wizardStep === checks.length - 1 ? 'Starting…' : 'Saving…';
      let advance = true;
      try {
      if (wizardStep === 0) {
        const type = dialog.querySelector('[data-wizard-receiver]').value;
        if (type !== config.capture?.device?.type) await api.switchReceiver(type);
        dialog.querySelectorAll('[data-receiver-field]').forEach(input => {
          const path = input.dataset.receiverField.split('.');
          const numeric = input.type === 'number';
          api.set(['capture', 'device', ...path], numeric ? Number(input.value) : input.value.trim());
        });
        if (!receiverHardwareReady(config.capture?.device))
          throw new Error('Complete the required receiver hardware fields before continuing.');
        wizardVisited.add(0);
      } else if (wizardStep === 1) {
        api.set(['capture','fc'], Number(dialog.querySelector('[data-wizard-fc]').value) * 1e6);
        api.set(['capture','fs'], Number(dialog.querySelector('[data-wizard-fs]').value) * 1e6);
        wizardVisited.add(1);
      } else if (wizardStep === 2) {
        dialog.querySelectorAll('[data-site]').forEach(input => {
          const [role, key] = input.dataset.site.split('.');
          api.set(['location', role, key], key === 'name' ? input.value.trim() : Number(input.value));
        });
        wizardVisited.add(2);
      } else if (wizardStep === 3) {
        api.set(['process','data','cpi'], Number(dialog.querySelector('[data-wizard-cpi]').value));
        const result = searchWindow(config, Number(dialog.querySelector('[data-wizard-range]').value), Number(dialog.querySelector('[data-wizard-speed]').value));
        if (result) { api.set(['process','ambiguity','delayMax'], result.delayMax); api.set(['process','ambiguity','dopplerMin'], -result.dopplerMax); api.set(['process','ambiguity','dopplerMax'], result.dopplerMax); }
        wizardVisited.add(3);
      } else if (wizardStep === 4) {
        wizardVisited.add(4);
      } else {
        const ready = wizardChecks(config, api.receiverProfiles()).slice(0, 5)
          .every(item => item.complete);
        if (!ready) throw new Error('Complete every required setup step before starting the radar.');
        const automatic = wizardAutomatic || recommendedAutomatic();
        api.set(['process','detection','enable'], automatic.detection);
        api.set(['process','tracker','enable'], automatic.tracker);
        api.set(['truth','adsb','enabled'], automatic.adsb);
        if (config.capture?.aircraft_clips)
          api.set(['capture','aircraft_clips','enabled'], automatic.clips);
        const saved = await api.save();
        if (saved === false) throw new Error('VectorWarp could not apply the completed setup. Review the error above and try again.');
        close(); advance = false;
      }
      if (advance) {
        const saved = await api.saveStep();
        if (saved === false) throw new Error('This step could not be saved. Correct the highlighted setting and try again.');
        wizardStep += 1;
        api.refresh();
      }
      } catch (error) {
        wizardError = error?.message || 'This setup step could not be saved.';
        api.refresh();
      } finally { wizardBusy = false; }
    });
    if (typeof dialog.showModal === 'function') dialog.showModal(); else dialog.setAttribute('open', '');
  }
  function render(target, api) {
    if (!target) return;
    const config = api.getConfig();
    const checks = wizardChecks(config, api.receiverProfiles()).slice(0, 5);
    const ready = checks.filter(item => item.complete).length;
    const receiver = api.receiverProfiles().find(profile =>
      profile.type === config.capture?.device?.type);
    const frequency = Number(config.capture?.fc || 0) / 1e6;
    const count = channels(config);
    const complete = ready === checks.length;
    const status = complete ? 'Setup complete' : `${ready} of ${checks.length} setup steps ready`;
    const summary = `${escape(receiver?.label || config.capture?.device?.type || 'No receiver')} · ${frequency.toFixed(3)} MHz · ${count} channel${count === 1 ? '' : 's'}`;
    target.innerHTML = `<section class="setup-assistant setup-assistant-launcher setup-assistant-summary"><div><strong>${status}</strong><span>${summary}</span></div><button type="button" class="button-secondary" data-start-setup>${complete ? 'Review setup' : 'Finish setup'}</button></section>`;
    target.querySelector('[data-start-setup]').addEventListener('click', async event => {
      const button = event.currentTarget;
      if (complete) {
        wizardOpen = true;
        wizardAutomatic = recommendedAutomatic();
        wizardStep = checks.length;
        api.refresh();
        return;
      }
      button.disabled = true; button.textContent = 'Looking for SDR…';
      wizardError = ''; wizardNotice = '';
      try {
        const detected = typeof api.discoverReceivers === 'function' ?
          await api.discoverReceivers() : [];
        if (detected.length === 1) {
          const receiver = detected[0];
          if (receiver.type !== api.getConfig().capture?.device?.type)
            await api.switchReceiver(receiver.type);
          wizardNotice = `${receiver.label || receiver.type} was detected and selected automatically.`;
        } else if (detected.length > 1) {
          wizardNotice = `${detected.length} supported receivers were detected. Choose the one to use.`;
        } else wizardNotice = 'No supported receiver was detected automatically. Choose one to configure manually.';
      } catch (error) {
        wizardNotice = 'Automatic receiver detection was unavailable. You can still choose a receiver manually.';
      }
      wizardOpen = true;
      wizardAutomatic = recommendedAutomatic();
      const refreshedChecks = wizardChecks(api.getConfig(), api.receiverProfiles()).slice(0, 5);
      wizardStep = refreshedChecks.findIndex(item => !item.complete);
      if (wizardStep < 0) wizardStep = refreshedChecks.length;
      api.refresh();
    });
    mountWizard(target, api);
  }
  root.VectorWarpSetupAssistant = {render, recordingEstimate, searchWindow, gpuActive,
    receiverHardwareReady, wizardChecks};
  if (typeof module !== 'undefined' && module.exports)
    module.exports = {render, recordingEstimate, searchWindow, gpuActive,
      receiverHardwareReady, wizardChecks};
})(typeof window !== 'undefined' ? window : globalThis);
