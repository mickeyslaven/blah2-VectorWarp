(function (root) {
  'use strict';
  const bytes = value => {
    if (!Number.isFinite(value)) return '—';
    const units = ['B', 'KB', 'MB', 'GB', 'TB'];
    let amount = value, unit = 0;
    while (amount >= 1024 && unit < units.length - 1) { amount /= 1024; unit += 1; }
    return `${amount.toFixed(unit < 2 ? 0 : 1)} ${units[unit]}`;
  };
  const duration = seconds => !Number.isFinite(seconds) ? '—' :
    recordingDuration(Math.round(seconds * 1000));
  const frequency = value => Number.isFinite(value) ? `${(value / 1e6).toFixed(3)} MHz` : '—';
  const escape = value => String(value ?? '').replace(/[&<>"']/g, character =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;', "'":'&#39;'}[character]));
  let indexTimer = null;
  const aircraft = item => item.aircraft?.length ? item.aircraft.map(value => {
    const name = value.flight || value.icao || 'Aircraft';
    const details = [value.icao && value.flight ? value.icao : null,
      Number.isFinite(value.slant_range_km) ? `${value.slant_range_km.toFixed(1)} km` : null,
      Number.isFinite(value.elevation_deg) ? `${value.elevation_deg.toFixed(1)}° elevation` : null]
      .filter(Boolean).join(' · ');
    return `<span class="recording-aircraft"><strong>${escape(name)}</strong>${details ? `<small>${escape(details)}</small>` : ''}</span>`;
  }).join('') : '<span class="recording-aircraft"><strong>No aircraft label</strong><small>Manual recording or unavailable ADS-B metadata</small></span>';
  function card(item) {
    const replay = `/display/configuration?replayFile=${encodeURIComponent(item.replay_file || '')}`;
    return `<article class="recording-card"><header><div><h2>${escape(item.title)}</h2><p>${escape(item.filename)}</p></div><span class="recording-kind ${escape(item.kind)}">${item.kind === 'automatic' ? 'Aircraft clip' : 'Manual'}</span></header><div class="recording-aircraft-list">${aircraft(item)}</div><dl><div><dt>Recorded</dt><dd>${escape(new Date(item.created_at).toLocaleString())}</dd></div><div><dt>Length</dt><dd>${escape(duration(item.duration_seconds))}</dd></div><div><dt>Size</dt><dd>${escape(bytes(item.size_bytes))}</dd></div><div><dt>Frequency</dt><dd>${escape(frequency(item.frequency_hz))}</dd></div><div><dt>Receiver</dt><dd>${escape(item.receiver || '—')}</dd></div><div><dt>Channels</dt><dd>${escape(item.channels ?? '—')}</dd></div></dl><footer><a class="button-link" href="${replay}">Load for replay</a><a class="button-link button-secondary" href="${escape(liveApiUrl(`/api/recordings/${item.id}/file`))}">Download</a>${item.active ? '<strong class="recording-active">Recording now</strong>' : ''}</footer></article>`;
  }
  async function load() {
    const list = document.getElementById('recording-list');
    const summary = document.getElementById('recording-library-summary');
    const search = document.getElementById('recording-search')?.value || '';
    try {
      const response = await fetchStatusResource(liveApiUrl(`/api/recordings?limit=200&search=${encodeURIComponent(search)}`), {cache: 'no-store'});
      if (!response.ok) throw new Error('Recording library unavailable');
      const result = await response.json();
      const storage = result.storage || {}, retention = result.retention || {};
      const limits = [retention.maximumAgeDays > 0 ? `${retention.maximumAgeDays} days` : null,
        retention.maximumTotalGb > 0 ? `${retention.maximumTotalGb} GB` : null]
        .filter(Boolean).join(' or ');
      const lowSpace = retention.lowSpacePolicy === 'delete_oldest' ?
        'Delete oldest aircraft clips' : 'Stop recording';
      summary.innerHTML = `<div><strong>${result.indexing ? 'Indexing recordings…' : `${result.total} recording${result.total === 1 ? '' : 's'}`}</strong><span>${escape(bytes(storage.recording_bytes))} stored · ${escape(bytes(storage.free_bytes))} disk free</span></div><div><strong>${retention.automaticCleanup ? 'Automatic cleanup on' : 'Automatic cleanup off'}</strong><span>${retention.automaticCleanup ? `Keep up to ${escape(limits || 'configured limits')}; reserve ${escape(retention.minimumFreeGb)} GB free` : `${escape(lowSpace)} at the ${escape(retention.minimumFreeGb)} GB reserve`}</span></div>`;
      list.innerHTML = result.recordings?.length ? result.recordings.map(card).join('') :
        '<div class="empty-state"><strong>No matching recordings</strong><p>Record manually from Live radar or enable automatic aircraft clips in Settings.</p></div>';
      clearTimeout(indexTimer);
      if (result.indexing) indexTimer = setTimeout(load, 1500);
    } catch (error) {
      summary.textContent = 'Recording storage is unavailable.';
      list.innerHTML = `<div class="empty-state"><strong>Could not load recordings</strong><p>${escape(error.message)}</p><button type="button" id="recording-retry">Try again</button></div>`;
      document.getElementById('recording-retry')?.addEventListener('click', load);
    }
  }
  function renderRecordings() {
    let timer = null;
    document.getElementById('recording-refresh')?.addEventListener('click', load);
    document.getElementById('recording-search')?.addEventListener('input', () => {
      clearTimeout(timer); timer = setTimeout(load, 250);
    });
    load();
  }
  root.renderRecordings = renderRecordings;
  if (typeof module !== 'undefined' && module.exports) module.exports = {bytes, duration, frequency};
})(typeof window !== 'undefined' ? window : globalThis);
