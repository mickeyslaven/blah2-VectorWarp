(function (root) {
  'use strict';
  const pageSize = () => typeof window !== 'undefined' && window.innerWidth <= 760 ? 10 : 24;
  const state = {offset: 0, total: 0, loading: false, controller: null,
    requestId: 0, indexTimer: null, rendered: false, importRequest: null};
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
    const replay = `/display/configuration?replayFile=${encodeURIComponent(item.replay_file || '')}&replayId=${encodeURIComponent(item.id || '')}`;
    return `<details class="recording-card" data-recording-id="${escape(item.id)}"${expanded}><summary><span><strong>${escape(item.title)}</strong><small>${escape(quick)}</small></span><span class="recording-kind ${escape(item.kind)}">${item.kind === 'automatic' ? 'Aircraft clip' : 'Manual'}</span></summary><div class="recording-card-body"><p class="recording-filename">${escape(item.filename)}</p><div class="recording-aircraft-list">${aircraft(item)}</div><dl><div><dt>Recorded</dt><dd>${escape(new Date(item.created_at).toLocaleString())}</dd></div><div><dt>Length</dt><dd>${escape(duration(item.duration_seconds))}</dd></div><div><dt>Size</dt><dd>${escape(bytes(item.size_bytes))}</dd></div><div><dt>Frequency</dt><dd>${escape(frequency(item.frequency_hz))}</dd></div><div><dt>Receiver</dt><dd>${escape(item.receiver || '—')}</dd></div><div><dt>Channels</dt><dd>${escape(item.channels ?? '—')}</dd></div></dl><footer><a class="button-link recording-replay" href="${escape(replay)}">Replay recording</a><button class="button-secondary recording-share" type="button" data-id="${escape(item.id)}" data-name="${escape(item.filename)}" ${item.active ? 'disabled title="Finish recording before sharing"' : ''}>Share recording</button><button class="button-secondary recording-delete" type="button" data-id="${escape(item.id)}" data-name="${escape(item.filename)}" ${item.active ? 'disabled' : ''}>Delete</button>${integrity(item)}${item.active ? '<strong class="recording-active">Recording now</strong>' : ''}</footer></div></details>`;
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
    const indexing = result.indexing || result.metadata_indexing;
    document.getElementById('recording-page-status').textContent =
      indexing && !result.recordings.length ? 'Loading recordings… The local library is still being indexed.' :
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
      const recordingsUrl = liveApiUrl(`/api/recordings?${query()}`);
      let result;
      if (typeof fetchCachedJsonResource === 'function') {
        result = await fetchCachedJsonResource(recordingsUrl, {signal: controller.signal},
          {freshMs: 1000, staleMs: 3600000, storage: 'session', timeoutMs: 15000,
            awaitFresh: true});
      } else {
        const response = await fetchStatusResource(recordingsUrl,
          {cache: 'no-store', signal: controller.signal}, 15000);
        if (!response.ok) throw new Error('Recording library unavailable');
        result = await response.json();
      }
      if (requestId !== state.requestId) return;
      state.total = result.total;
      const recordings = Array.isArray(result.recordings) ? result.recordings : [];
      const indexError = String(result.index_error || '').trim();
      if (indexError && !recordings.length)
        throw new Error(`Recording library scan failed: ${indexError}`);
      const storage = result.storage || {}, retention = result.retention || {};
      const limits = [retention.maximumAgeDays > 0 ? `${retention.maximumAgeDays} days` : null,
        retention.maximumTotalGb > 0 ? `${retention.maximumTotalGb} GB` : null].filter(Boolean).join(' or ');
      const lowSpace = retention.lowSpacePolicy === 'delete_oldest' ? 'Delete oldest aircraft clips' : 'Stop recording';
      summary.innerHTML = `<div><strong>${indexError ? 'Showing the last available recording index' : result.indexing ? 'Indexing recordings…' : `${result.total} matching recording${result.total === 1 ? '' : 's'}`}</strong><span>${indexError ? `Refresh delayed: ${escape(indexError)}` : `${escape(bytes(storage.recording_bytes))} stored · ${escape(bytes(storage.free_bytes))} disk free`}</span></div><div><strong>${retention.automaticCleanup ? 'Automatic cleanup on' : 'Automatic cleanup off'}</strong><span>${retention.automaticCleanup ? `Keep up to ${escape(limits || 'configured limits')}; reserve ${escape(retention.minimumFreeGb)} GB free` : `${escape(lowSpace)} at the ${escape(retention.minimumFreeGb)} GB reserve`}</span></div>`;
      const indexing = result.indexing || result.metadata_indexing;
      list.innerHTML = recordings.length ? recordings.map(card).join('') : indexing ?
        '<div class="empty-state recording-loading-state"><strong>Loading recordings…</strong><p>Indexing the local recording library and aircraft labels.</p></div>' :
        '<div class="empty-state"><strong>No matching recordings</strong><p>Change the filters, record manually, import a recording, or enable automatic aircraft clips.</p></div>';
      state.rendered = true;
      pageControls(result);
      // Metadata indexing is background work.  One slow, owned refresh is
      // enough; stacked timers used to abort each other and intermittently
      // replace a valid library with an error on mobile browsers.
      if ((result.indexing || result.metadata_indexing) && !document.hidden)
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
  function confirmDelete(name) {
    const dialog = document.getElementById('recording-delete-dialog');
    if (!dialog || typeof dialog.showModal !== 'function')
      return Promise.resolve(window.confirm(`Permanently delete ${name} and its metadata?`));
    document.getElementById('recording-delete-copy').textContent = name;
    return new Promise(resolve => {
      const done = () => { dialog.removeEventListener('close', done); resolve(dialog.returnValue === 'confirm'); };
      dialog.addEventListener('close', done); dialog.showModal();
    });
  }
  async function deleteRecording(button) {
    const name = button.dataset.name || 'this recording';
    if (!(await confirmDelete(name))) return;
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
  async function shareRecording(button) {
    const name = button.dataset.name || 'recording';
    const status = document.getElementById('recording-operation-status');
    button.disabled = true; button.textContent = 'Preparing…';
    try {
      const response = await fetchStatusResource(liveApiUrl(`/api/recordings/${button.dataset.id}/share`),
        {method: 'POST', headers: {'X-VectorWarp-Intent': 'recording-share-v1'}, cache: 'no-store'}, 15000);
      const body = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(body.error || 'The recording could not be shared.');
      if (body.action === 'revealed') {
        if (status) status.textContent = body.selected === false ?
          `Opened the folder containing ${name}.` : `Opened ${name} in the recording folder.`;
      } else if (body.action === 'download' && body.url) {
        const link = document.createElement('a');
        link.href = liveApiUrl(body.url); link.download = ''; link.hidden = true;
        document.body.appendChild(link); link.click(); link.remove();
        if (status) status.textContent = `Downloading ${name} with its replay settings, ADS-B timeline, and metadata.`;
      } else throw new Error('The sharing response was incomplete.');
    } catch (error) {
      if (status) status.textContent = `${error.message || 'Sharing failed'} Try again.`;
    } finally { button.disabled = false; button.textContent = 'Share recording'; }
  }
  async function importRecording(input) {
    const file = input.files?.[0]; if (!file) return;
    const status = document.getElementById('recording-import-status');
    const progress = document.getElementById('recording-import-progress');
    const cancel = document.getElementById('recording-import-cancel');
    status.textContent = `Importing ${file.name} (${bytes(file.size)})…`;
    progress.hidden = false; progress.value = 0; cancel.hidden = false; input.disabled = true;
    try {
      const result = await new Promise((resolve, reject) => {
        const request = new XMLHttpRequest(); state.importRequest = request;
        request.open('POST', liveApiUrl(`/api/recordings/import?filename=${encodeURIComponent(file.name)}`));
        request.timeout = 60 * 60 * 1000;
        request.setRequestHeader('Content-Type', 'application/octet-stream');
        request.setRequestHeader('X-VectorWarp-Intent', 'recording-import-v1');
        request.upload.onprogress = event => {
          if (!event.lengthComputable) return;
          progress.value = Math.round(event.loaded / event.total * 100);
          status.textContent = `Importing ${file.name}: ${progress.value}%`;
        };
        request.onload = () => {
          let body = {}; try { body = JSON.parse(request.responseText || '{}'); } catch (_) {}
          if (request.status < 200 || request.status >= 300) reject(new Error(body.error || 'Import failed'));
          else resolve(body);
        };
        request.onerror = () => reject(new Error('Import connection failed'));
        request.ontimeout = () => reject(new Error('Import timed out'));
        request.onabort = () => reject(new DOMException('Import cancelled', 'AbortError'));
        request.send(file);
      });
      status.textContent = `Imported ${result.filename} (${bytes(result.size_bytes)}).`;
      input.value = ''; await load({reset: true});
    } catch (error) { status.textContent = error.name === 'AbortError' ? 'Import cancelled.' : error.message; }
    finally { state.importRequest = null; progress.hidden = true; cancel.hidden = true; input.disabled = false; }
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
      const share = event.target.closest('.recording-share'); if (share) { shareRecording(share); return; }
      const remove = event.target.closest('.recording-delete'); if (remove) deleteRecording(remove);
    });
    document.getElementById('recording-import')?.addEventListener('change', event => importRecording(event.target));
    document.getElementById('recording-import-cancel')?.addEventListener('click', () => state.importRequest?.abort());
    load();
  }
  root.renderRecordings = renderRecordings;
  if (typeof module !== 'undefined' && module.exports)
    module.exports = {bytes, duration, frequency, pageSize};
})(typeof window !== 'undefined' ? window : globalThis);
