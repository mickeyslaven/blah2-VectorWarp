'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const {pipeline} = require('stream/promises');
const {descriptor: recordingPackageDescriptor, streamPackage: streamRecordingPackage,
  extractPackage: extractRecordingPackage} = require('./recording-package.js');
const {isLocalDeviceRequest, reveal: revealRecording} = require('./recording-share.js');

function installRecordingLibraryRoutes(app, {
  recordingLibrary, recordingTimeline, recordingPasses, activeRecordingFile,
  currentProcessor, configWriteOriginAllowed
}) {
  const publicRecording = item => {
    const {path: replay_file, ...value} = item;
    return {...value, replay_file};
  };
  const recordingNotFound = res => res.status(404).json({error: 'Recording not found.'});

  app.get('/api/recordings', async (req, res, next) => {
    try {
      const processor = currentProcessor();
      const offset = Number.parseInt(req.query.offset, 10);
      const limit = Number.parseInt(req.query.limit, 10);
      const result = await recordingLibrary.list({activeFile: activeRecordingFile(processor),
        passes: recordingPasses(), search: req.query.search, kind: req.query.kind,
        receiver: req.query.receiver, frequency: req.query.frequency, sort: req.query.sort,
        offset: Number.isInteger(offset) ? offset : 0,
        limit: Number.isInteger(limit) ? limit : undefined, waitForIndex: false});
      res.json({...result, recordings: result.recordings.map(publicRecording)});
    } catch (error) { next(error); }
  });
  app.get('/api/recordings/directories', async (req, res, next) => {
    try { res.json(await recordingLibrary.directories()); }
    catch (error) { next(error); }
  });
  app.get('/api/recordings/:id/replay-profile', async (req, res) => {
    const filename = recordingLibrary.resolve(req.params.id);
    if (!filename) return recordingNotFound(res);
    const timeline = recordingTimeline.load(filename);
    const packaged = recordingTimeline.loadReplayProfile(filename);
    let metadata = {};
    try { metadata = JSON.parse(await fs.promises.readFile(`${filename}.json`, 'utf8')); }
    catch (_) { /* Portable recordings may only have a provenance sidecar. */ }
    if (!metadata.channels) try {
      const provenance = JSON.parse(await fs.promises.readFile(`${filename}.provenance.json`, 'utf8'));
      metadata = {...metadata, channels: provenance.channels,
        sampleRate: provenance.sampleRateHz, frequency: provenance.frequencyHz,
        receiver: provenance.receiver};
    } catch (_) { /* Header validation remains the final replay authority. */ }
    const channels = timeline?.channels ?? metadata.channels ?? null;
    const legacy = timeline?.replay_profile || null;
    const profile = packaged || legacy;
    const source = profile?.capture || {};
    const referenceMode = profile?.process?.reference_synthesis?.mode ||
      (channels > 2 ? 'array_eigenbeam' : 'dedicated');
    const all = Number.isInteger(channels) ? Array.from({length: channels}, (_, index) => index) : [];
    res.json({file: filename, profile: profile || null,
      configuration_complete: packaged?.configuration_complete === true,
      limitations: packaged ? [] : ['This legacy recording does not contain a complete scientific configuration.'],
      sample_rate_hz: timeline?.sample_rate_hz ?? metadata.sampleRate ?? null,
      frequency_hz: timeline?.frequency_hz ?? metadata.frequency ?? null,
      channel_count: channels,
      reference_channel: source.reference_channel ?? 0,
      surveillance_channels: source.surveillance_channels ||
        (referenceMode === 'array_eigenbeam' ? all : all.filter(index => index !== 0)),
      reference_mode: referenceMode,
      reference_channels: profile?.process?.reference_synthesis?.channels ||
        (referenceMode === 'array_eigenbeam' ? all : [])});
  });
  app.post('/api/recordings/directories', async (req, res, next) => {
    if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-directory-v1')
      return res.status(403).json({error: 'Recording-folder changes require a trusted VectorWarp origin and intent.'});
    try { res.json(await recordingLibrary.createDirectory(req.body?.name)); }
    catch (error) { next(error); }
  });
  app.get('/api/recordings/:id/file', (req, res) => {
    const filename = recordingLibrary.resolve(req.params.id);
    if (!filename) return recordingNotFound(res);
    res.type('application/octet-stream');
    res.set('Content-Disposition', `attachment; filename="${path.basename(filename).replace(/["\\]/g, '_')}"`);
    res.sendFile(filename);
  });
  app.get('/api/recordings/:id/package', async (req, res, next) => {
    const filename = recordingLibrary.resolve(req.params.id);
    if (!filename) return recordingNotFound(res);
    const processor = currentProcessor();
    if (processor?.recording === true && path.resolve(processor.recordingFile || '') === filename)
      return res.status(409).json({error: 'Finish this recording before sharing it.'});
    try {
      const value = await recordingPackageDescriptor(filename);
      res.type('application/vnd.vectorwarp.recording');
      res.set('Content-Length', String(value.size));
      res.set('Content-Disposition', `attachment; filename="${value.downloadName.replace(/[^A-Za-z0-9._-]/g, '_')}"`);
      await streamRecordingPackage(value, res);
    } catch (error) {
      if (!res.headersSent) next(error);
      else res.destroy(error);
    }
  });
  app.post('/api/recordings/:id/share', async (req, res, next) => {
    if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-share-v1')
      return res.status(403).json({error: 'Sharing a recording requires a trusted VectorWarp origin and intent.'});
    const filename = recordingLibrary.resolve(req.params.id);
    if (!filename) return recordingNotFound(res);
    const processor = currentProcessor();
    if (processor?.recording === true && path.resolve(processor.recordingFile || '') === filename)
      return res.status(409).json({error: 'Finish this recording before sharing it.'});
    if (!isLocalDeviceRequest(req))
      return res.json({action: 'download', url: `/api/recordings/${encodeURIComponent(req.params.id)}/package`});
    try {
      const result = await revealRecording(filename);
      res.json({action: 'revealed', filename: path.basename(filename), selected: result.selected});
    } catch (error) { next(Object.assign(new Error(`Could not open the recording folder: ${error.message}`), {status: 500})); }
  });
  app.delete('/api/recordings/:id', async (req, res, next) => {
    if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-delete-v1')
      return res.status(403).json({error: 'Deleting a recording requires a trusted VectorWarp origin and intent.'});
    const processor = currentProcessor();
    try {
      const result = await recordingLibrary.remove(req.params.id,
        {activeFile: activeRecordingFile(processor)});
      if (!result.removed) return res.status(result.reason === 'active' ? 409 : 404).json(result);
      res.json(result);
    } catch (error) { next(error); }
  });
  app.post('/api/recordings/import', async (req, res, next) => {
    if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-import-v1')
      return res.status(403).json({error: 'Importing a recording requires a trusted VectorWarp origin and intent.'});
    const original = path.basename(String(req.query.filename || ''));
    const extension = path.extname(original).toLowerCase();
    if (!original || !['.blah2iq', '.mchq', '.vwr'].includes(extension))
      return res.status(400).json({error: 'Choose a .vwr package, .blah2iq recording, or .mchq clip.'});
    let temporary = null;
    try {
      const disk = await recordingLibrary.prepareForRecording({});
      const length = Number(req.get('Content-Length'));
      const requiredBytes = Number.isFinite(length) ? length * (extension === '.vwr' ? 2 : 1) : 0;
      if (!disk.allowed || (requiredBytes && disk.free_bytes !== null &&
          disk.free_bytes - requiredBytes <= disk.required_free_bytes))
        return res.status(507).json({error: 'The import would cross the configured free-disk reserve.'});
      const directory = recordingLibrary.directory();
      await fs.promises.mkdir(directory, {recursive: true, mode: 0o770});
      temporary = path.join(directory, `.import-${crypto.randomUUID()}.partial`);
      await pipeline(req, fs.createWriteStream(temporary, {flags: 'wx', mode: 0o660}));
      const stat = await fs.promises.stat(temporary);
      if (!stat.size) throw Object.assign(new Error('The selected recording was empty.'), {status: 400});
      let imported;
      if (extension === '.vwr') {
        imported = await extractRecordingPackage(temporary, directory);
        await fs.promises.rm(temporary, {force: true}); temporary = null;
      } else {
        const stem = path.basename(original, extension).replace(/[^A-Za-z0-9._-]+/g, '-').slice(0, 120) || 'recording';
        let filename = path.join(directory, `${stem}${extension}`);
        if (fs.existsSync(filename)) filename = path.join(directory, `${stem}-${Date.now()}${extension}`);
        await fs.promises.rename(temporary, filename); temporary = null;
        imported = {filename: path.basename(filename), size_bytes: stat.size, files: [path.basename(filename)]};
      }
      recordingLibrary.cache = null;
      res.status(201).json({imported: true, packaged: extension === '.vwr', ...imported});
    } catch (error) {
      if (temporary) await fs.promises.rm(temporary, {force: true}).catch(() => {});
      next(error);
    }
  });
  app.post('/api/recordings/cleanup', async (req, res, next) => {
    if (!configWriteOriginAllowed(req) || req.get('X-VectorWarp-Intent') !== 'recording-cleanup-v1')
      return res.status(403).json({error: 'Recording cleanup requires a trusted VectorWarp origin and intent.'});
    const processor = currentProcessor();
    try { res.json(await recordingLibrary.cleanup({activeFile: activeRecordingFile(processor), force: true})); }
    catch (error) { next(error); }
  });
}

module.exports = {installRecordingLibraryRoutes};
