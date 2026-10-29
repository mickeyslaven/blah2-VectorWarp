(function (root) {
  'use strict';
  const pageSize = () => typeof window !== 'undefined' && window.innerWidth <= 760 ? 10 : 24;
  const state = {offset: 0, total: 0, loading: false, controller: null,
    requestId: 0, indexTimer: null, rendered: false};
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
  const aircraft = item => item.aircraft?.length ? item.aircraft.map(value => {
    const name = value.flight || value.icao || 'Aircraft';
    const details = [value.icao && value.flight ? value.icao : null,
      Number.isFinite(value.slant_range_km) ? `${value.slant_range_km.toFixed(1)} km` : null,
      Number.isFinite(value.elevation_deg) ? `${value.elevation_deg.toFixed(1)}° elevation` : null]
      .filter(Boolean).join(' · ');
    return `<span class="recording-aircraft"><strong>${escape(name)}</strong>${details ? `<small>${escape(details)}</small>` : ''}</span>`;
  }).join('') : '<span class="recording-aircraft"><strong>No aircraft label</strong><small>Manual recording or unavailable ADS-B metadata</small></span>';
  const integrity = item => item.complete === false ? '<span class="recording-integrity bad">Incomplete</span>' :
    item.admission ? `<span class="recording-integrity">${escape(item.admission)}</span>` :
      '<span class="recording-integrity">Portable recording</span>';
  function card(item) {
    const expanded = typeof window === 'undefined' || window.innerWidth > 760 ? ' open' : '';
    const quick = [duration(item.duration_seconds), bytes(item.size_bytes), frequency(item.frequency_hz)].filter(value => value !== '—').join(' · ');
    const replay = `/display/configuration?replayFile=${encodeURIComponent(item.replay_file || '')}`;
    return `<details class="recording-card" data-recording-id="${escape(item.id)}"${expanded}><summary><span><strong>${escape(item.title)}</strong><small>${escape(quick)}</small></span><span class="recording-kind ${escape(item.kind)}">${item.kind === 'automatic' ? 'Aircraft clip' : 'Manual'}</span></summary><div class="recording-card-body"><p class="recording-filename">${escape(item.filename)}</p><div class="recording-aircraft-list">${aircraft(item)}</div><dl><div><dt>Recorded</dt><dd>${escape(new Date(item.created_at).toLocaleString())}</dd></div><div><dt>Length</dt><dd>${escape(duration(item.duration_seconds))}</dd></div><div><dt>Size</dt><dd>${escape(bytes(item.size_bytes))}</dd></div><div><dt>Frequency</dt><dd>${escape(frequency(item.frequency_hz))}</dd></div><div><dt>Receiver</dt><dd>${escape(item.receiver || '—')}</dd></div><div><dt>Channels</dt><dd>${escape(item.channels ?? '—')}</dd></div></dl><footer><a class="button-link recording-replay" href="${escape(replay)}">Review replay</a><button class="button-secondary recording-delete" type="button" data-id="${escape(item.id)}" data-name="${escape(item.filename)}" ${item.active ? 'disabled' : ''}>Delete</button>${integrity(item)}${item.active ? '<strong class="recording-active">Recording now</strong>' : ''}</footer></div></details>`;
  }
  function query() {
    const params = new URLSearchParams({offset: state.offset, limit: pageSize(),
      search: document.getElementById('recording-search')?.value || '',
      kind: document.getElementById('recording-kind')?.value || '',
      sort: document.getElementById('recording-sort')?.value || 'newest'});
    return params;
  }
  function pageControls(result) {
    const start = result.total ? result.offset + 1 : 0;
    const end = Math.min(result.total, result.offset + result.recordings.length);
    document.getElementById('recording-page-status').textContent =
      result.metadata_indexing ? `Showing ${start}–${end} of ${result.total}. Indexing aircraft metadata for complete search…` :
        `Showing ${start}–${end} of ${result.total}`;
    document.getElementById('recording-page').textContent =
      `Page ${result.total ? Math.floor(result.offset / pageSize()) + 1 : 1} of ${Math.max(1, Math.ceil(result.total / pageSize()))}`;
    document.getElementById('recording-previous').disabled = result.offset <= 0;
    document.getElementById('recording-next').disabled = result.offset + result.recordings.length >= result.total;
  }
  async function load({reset = false} = {}) {
    if (state.indexTimer) { window.clearTimeout(state.indexTimer); state.indexTimer = null; }
    state.controller?.abort();
    if (reset) state.offset = 0;
    const requestId = ++state.requestId;
    const controller = new AbortController();
    state.controller = controller; state.loading = true;
    const list = document.getElementById('recording-list');
    const summary = document.getElementById('recording-library-summary');
    list?.classList.add('is-loading');
    list?.setAttribute('aria-busy', 'true');
    const pageStatus = document.getElementById('recording-page-status');
    if (pageStatus) pageStatus.textContent = 'Loading recordings…';
    try {
      const response = await fetchStatusResource(liveApiUrl(`/api/recordings?${query()}`),
        {cache: 'no-store', signal: controller.signal}, 15000);
      if (!response.ok) throw new Error('Recording library unavailable');
      const result = await response.json();
      if (requestId !== state.requestId) return;
      state.total = result.total;
      const storage = result.storage || {}, retention = result.retention || {};
      const limits = [retention.maximumAgeDays > 0 ? `${retention.maximumAgeDays} days` : null,
        retention.maximumTotalGb > 0 ? `${retention.maximumTotalGb} GB` : null].filter(Boolean).join(' or ');
      const lowSpace = retention.lowSpacePolicy === 'delete_oldest' ? 'Delete oldest aircraft clips' : 'Stop recording';
      summary.innerHTML = `<div><strong>${result.indexing ? 'Indexing recordings…' : `${result.total} matching recording${result.total === 1 ? '' : 's'}`}</strong><span>${escape(bytes(storage.recording_bytes))} stored · ${escape(bytes(storage.free_bytes))} disk free</span></div><div><strong>${retention.automaticCleanup ? 'Automatic cleanup on' : 'Automatic cleanup off'}</strong><span>${retention.automaticCleanup ? `Keep up to ${escape(limits || 'configured limits')}; reserve ${escape(retention.minimumFreeGb)} GB free` : `${escape(lowSpace)} at the ${escape(retention.minimumFreeGb)} GB reserve`}</span></div>`;
      list.innerHTML = result.recordings?.length ? result.recordings.map(card).join('') :
        '<div class="empty-state"><strong>No matching recordings</strong><p>Change the filters, record manually, import a recording, or enable automatic aircraft clips.</p></div>';
      state.rendered = true;
      pageControls(result);
      // Metadata indexing is background work.  One slow, owned refresh is
      // enough; stacked timers used to abort each other and intermittently
      // replace a valid library with an error on mobile browsers.
      if (result.indexing && !document.hidden)
        state.indexTimer = window.setTimeout(() => load(), 5000);
    } catch (error) {
      if (error?.name === 'AbortError' || requestId !== state.requestId) return;
      if (state.rendered) {
        summary.innerHTML = '<div><strong>Refresh delayed</strong><span>The existing recording list is still available. Retrying shortly…</span></div>';
        state.indexTimer = window.setTimeout(() => load(), 3000);
      } else {
        summary.textContent = 'Recording storage is unavailable.';
        list.innerHTML = `<div class="empty-state"><strong>Could not load recordings</strong><p>${escape(error.message)}</p><button type="button" id="recording-retry">Try again</button></div>`;
        document.getElementById('recording-retry')?.addEventListener('click', () => load());
      }
    } finally {
      if (requestId === state.requestId) {
        state.loading = false;
        state.controller = null;
        list?.classList.remove('is-loading');
        list?.setAttribute('aria-busy', 'false');
      }
    }
  }
  async function deleteRecording(button) {
    const name = button.dataset.name || 'this recording';
    if (!window.confirm(`Permanently delete ${name} and its metadata?`)) return;
    button.disabled = true; button.textContent = 'Deleting…';
    const status = document.getElementById('recording-operation-status');
    try {
      const response = await fetchStatusResource(liveApiUrl(`/api/recordings/${button.dataset.id}`),
        {method: 'DELETE', headers: {'X-VectorWarp-Intent': 'recording-delete-v1'}, cache: 'no-store'}, 15000);
      if (!response.ok) throw new Error('The recording could not be deleted.');
      if (state.offset >= Math.max(0, state.total - 1)) state.offset = Math.max(0, state.offset - pageSize());
      if (status) status.textContent = `${name} was deleted.`;
      await load();
    } catch (error) {
      button.disabled = false;
      button.textContent = 'Delete';
      if (status) status.textContent = `${error.message || 'Delete failed'} The recording list was not changed; try again.`;
    }
  }
  async function importRecording(input) {
    const file = input.files?.[0]; if (!file) return;
    const status = document.getElementById('recording-import-status');
    status.textContent = `Importing ${file.name} (${bytes(file.size)})…`;
    try {
      const response = await fetchStatusResource(liveApiUrl(`/api/recordings/import?filename=${encodeURIComponent(file.name)}`),
        {method: 'POST', headers: {'Content-Type': 'application/octet-stream',
          'X-VectorWarp-Intent': 'recording-import-v1'}, body: file}, 60 * 60 * 1000);
      const result = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(result.error || 'Import failed');
      status.textContent = `Imported ${result.filename} (${bytes(result.size_bytes)}).`;
      input.value = ''; await load({reset: true});
    } catch (error) { status.textContent = error.message; }
  }
  function renderRecordings() {
    let timer = null;
    document.getElementById('recording-refresh')?.addEventListener('click', () => load());
    for (const id of ['recording-kind', 'recording-sort'])
      document.getElementById(id)?.addEventListener('change', () => load({reset: true}));
    document.getElementById('recording-search')?.addEventListener('input', () => {
      clearTimeout(timer); timer = setTimeout(() => load({reset: true}), 300);
    });
    document.getElementById('recording-previous')?.addEventListener('click', () => {
      state.offset = Math.max(0, state.offset - pageSize()); load(); window.scrollTo({top: 0, behavior: 'smooth'});
    });
    document.getElementById('recording-next')?.addEventListener('click', () => {
      state.offset += pageSize(); load(); window.scrollTo({top: 0, behavior: 'smooth'});
    });
    document.getElementById('recording-list')?.addEventListener('click', event => {
      const remove = event.target.closest('.recording-delete'); if (remove) deleteRecording(remove);
    });
    document.getElementById('recording-import')?.addEventListener('change', event => importRecording(event.target));
    load();
  }
  root.renderRecordings = renderRecordings;
  if (typeof module !== 'undefined' && module.exports)
    module.exports = {bytes, duration, frequency, pageSize};
})(typeof window !== 'undefined' ? window : globalThis);
