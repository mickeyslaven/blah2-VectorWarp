(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.KrakenGeometry = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
  'use strict';
  const prefix = 'capture.device.array_geometry';
  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const finite = value => typeof value === 'number' && Number.isFinite(value);
  const clone = value => JSON.parse(JSON.stringify(value));
  const confirmations = ['mapping_confirmed', 'geometry_confirmed', 'orientation_confirmed'];
  const shapes = ['custom', 'cross', 'regular_polygon', 'ula'];
  const lightSpeed = 299792458;
  const median = values => {
    if (!values.length) return null;
    const sorted = [...values].sort((left, right) => left - right);
    const middle = Math.floor(sorted.length / 2);
    return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
  };
  function layoutComparison(config) {
    const geometry = config?.capture?.device?.array_geometry;
    const frequencyHz = Number(config?.capture?.fc);
    const wavelengthM = frequencyHz > 0 ? lightSpeed / frequencyHz : null;
    const idealSpacingM = wavelengthM === null ? null : wavelengthM / 2;
    const bearing = new Set(Array.isArray(geometry?.bearing_channels) ? geometry.bearing_channels : []);
    let elements = Array.isArray(geometry?.elements) ? geometry.elements.filter(element =>
      Array.isArray(element?.position) && finite(element.position[0]) && finite(element.position[1])) : [];
    const selected = elements.filter(element => bearing.has(element.daq_channel));
    if (selected.length >= 2) elements = selected;
    const points = elements.map(element => ({id: element.id || `CH${element.daq_channel ?? '?'}`,
      channel: element.daq_channel, x: element.position[0], y: element.position[1]}));
    const nearest = points.map((point, index) => Math.min(...points
      .filter((_, other) => other !== index).map(other => Math.hypot(point.x - other.x, point.y - other.y))))
      .filter(finite);
    const radii = points.map(point => Math.hypot(point.x, point.y));
    const shape = geometry?.shape || 'custom';
    const idealRadiusM = idealSpacingM === null || points.length < 3 ? null :
      shape === 'cross' && points.length === 4 ? idealSpacingM / Math.sqrt(2) :
      shape === 'regular_polygon' ? idealSpacingM / (2 * Math.sin(Math.PI / points.length)) : null;
    let idealPoints = [];
    if (idealSpacingM !== null && points.length >= 2) {
      if (shape === 'ula') idealPoints = points.map((_, index) => ({
        x: (index - (points.length - 1) / 2) * idealSpacingM, y: 0
      }));
      else if (shape === 'cross' && points.length === 4) idealPoints = [
        {x: idealRadiusM, y: 0}, {x: 0, y: idealRadiusM},
        {x: -idealRadiusM, y: 0}, {x: 0, y: -idealRadiusM}
      ];
      else if (shape === 'regular_polygon' && idealRadiusM !== null)
        idealPoints = points.map((_, index) => ({
          x: Math.cos(2 * Math.PI * index / points.length) * idealRadiusM,
          y: Math.sin(2 * Math.PI * index / points.length) * idealRadiusM
        }));
    }
    return {frequencyHz, wavelengthM, idealSpacingM, idealRadiusM, points,
      idealPoints, actualSpacingM: median(nearest), actualRadiusM: median(radii), shape};
  }
  const metres = value => finite(value) ? `${value < .1 ? value.toFixed(3) : value.toFixed(3)} m` : '—';
  function comparisonVisual(document, config) {
    const model = layoutComparison(config);
    const shell = document.createElement('section');
    shell.className = 'geometry-visual';
    const copy = document.createElement('div');
    copy.className = 'geometry-visual-copy';
    const title = document.createElement('h3');
    title.textContent = 'Actual vs λ/2 reference layout';
    const explanation = document.createElement('p');
    explanation.textContent = model.wavelengthM === null ?
      'Set frequency to calculate λ/2 spacing.' :
      `${(model.frequencyHz / 1e6).toFixed(3)} MHz: λ/2 = ${metres(model.wavelengthM / 2)}.`;
    copy.append(title, explanation);
    const stats = document.createElement('dl');
    const row = (label, value, className = '') => {
      const wrapper = document.createElement('div');
      if (className) wrapper.className = className;
      const term = document.createElement('dt'); term.textContent = label;
      const detail = document.createElement('dd'); detail.textContent = value;
      wrapper.append(term, detail); stats.appendChild(wrapper);
    };
    row('Measured elements', String(model.points.length));
    row('Actual neighbour spacing', metres(model.actualSpacingM), 'actual');
    row('λ/2 reference spacing', metres(model.idealSpacingM), 'ideal');
    if (model.idealRadiusM !== null) {
      row('Actual median radius', metres(model.actualRadiusM), 'actual');
      row(`Reference radius (${model.points.length} elements)`, metres(model.idealRadiusM), 'ideal');
    } else if (model.shape === 'ula' && model.points.length >= 2) {
      row('Reference end-to-end length', metres(model.idealSpacingM * (model.points.length - 1)), 'ideal');
    }
    copy.appendChild(stats);
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 520 340');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', 'Top-down antenna array showing measured elements and half-wavelength reference geometry');
    const add = (name, attributes = {}, text = '') => {
      const node = document.createElementNS('http://www.w3.org/2000/svg', name);
      Object.entries(attributes).forEach(([key, value]) => node.setAttribute(key, String(value)));
      if (text) node.textContent = text;
      svg.appendChild(node); return node;
    };
    const centerX = 260, centerY = 170;
    const extent = Math.max(.001, ...model.idealPoints.map(point => Math.hypot(point.x, point.y)),
      ...model.points.map(point => Math.hypot(point.x, point.y)));
    const scale = 128 / extent;
    add('line', {x1: 50, y1: centerY, x2: 470, y2: centerY, class: 'geometry-axis'});
    add('line', {x1: centerX, y1: 30, x2: centerX, y2: 310, class: 'geometry-axis'});
    add('text', {x: 468, y: centerY - 8, class: 'geometry-axis-label'}, '+x');
    add('text', {x: centerX + 8, y: 42, class: 'geometry-axis-label'}, '+y');
    if (model.idealPoints.length >= 2) {
      const ideal = model.idealPoints.map(point =>
        `${centerX + point.x * scale},${centerY - point.y * scale}`).join(' ');
      add(model.shape === 'ula' ? 'polyline' : 'polygon', {points: ideal,
        class: 'geometry-ideal-ring'});
      model.idealPoints.forEach(point => add('circle', {
        cx: centerX + point.x * scale, cy: centerY - point.y * scale, r: 5,
        class: 'geometry-ideal-node'}));
    }
    if (model.points.length >= 2) {
      const ordered = [...model.points].sort(model.shape === 'ula' ?
        ((left, right) => left.x - right.x || left.y - right.y) :
        ((left, right) => Math.atan2(left.y, left.x) - Math.atan2(right.y, right.x)));
      const polygon = ordered.map(point => `${centerX + point.x * scale},${centerY - point.y * scale}`).join(' ');
      add(model.shape === 'ula' ? 'polyline' : 'polygon', {points: polygon,
        class: 'geometry-actual-outline'});
    }
    model.points.forEach(point => {
      const x = centerX + point.x * scale, y = centerY - point.y * scale;
      add('circle', {cx: x, cy: y, r: 11, class: 'geometry-actual-node'});
      add('text', {x, y: y + 4, class: 'geometry-node-label'},
        Number.isInteger(point.channel) ? `CH${point.channel}` : point.id.slice(0, 5));
    });
    if (!model.points.length)
      add('text', {x: centerX, y: centerY, class: 'geometry-empty-label'},
        'Enter measured x/y positions to draw the actual array');
    const legend = document.createElement('p');
    legend.className = 'geometry-visual-legend';
    legend.innerHTML = '<span class="actual">Measured layout</span><span class="ideal">λ/2 reference</span>';
    const drawing = document.createElement('div');
    drawing.className = 'geometry-visual-drawing';
    drawing.append(svg, legend);
    shell.append(drawing, copy);
    return shell;
  }
  function layoutPresets() {
    const presets = [];
    const polygonName = count => ({3: 'triangle', 5: 'pentagon', 6: 'hexagon',
      7: 'heptagon ring', 8: 'octagon ring'}[count]);
    for (let bearing = 2; bearing <= 8; bearing++)
      presets.push({id: `ula${bearing}`, title: `${bearing}-element line`,
        channels: bearing, bearing, shape: 'ula'});
    for (let bearing = 2; bearing <= 7; bearing++)
      presets.push({id: `ula${bearing}ref`, title: `${bearing}-element line + reference`,
        channels: bearing + 1, bearing, shape: 'ula', dedicated: true});
    for (const bearing of [3, 5, 6, 7, 8])
      presets.push({id: `${polygonName(bearing).split(' ')[0]}${bearing}`,
        title: `${bearing}-element ${polygonName(bearing)}`, channels: bearing,
        bearing, shape: 'regular_polygon'});
    for (const bearing of [3, 5, 6, 7])
      presets.push({id: `${polygonName(bearing).split(' ')[0]}${bearing}ref`,
        title: `${bearing}-element ${polygonName(bearing)} + reference`,
        channels: bearing + 1, bearing, shape: 'regular_polygon', dedicated: true});
    presets.push({id: 'cross4', title: '4-element cross', channels: 4,
      bearing: 4, shape: 'cross'});
    presets.push({id: 'cross4ref', title: '4-element cross + reference', channels: 5,
      bearing: 4, shape: 'cross', dedicated: true});
    return presets.map(item => Object.freeze(item));
  }
  function disableIncompatibleArrayFeatures(config, dedicated = false) {
    const process = config.process || {};
    if (process.surveillance_fusion &&
        ['coherent_beam_bank', 'noise_whitened_beam_bank'].includes(process.surveillance_fusion.mode))
      process.surveillance_fusion.mode = 'noncoherent';
    if (process.bearing) process.bearing.enable = false;
    if (process.detection) {
      if (process.detection.track_before_detect) process.detection.track_before_detect.enable = false;
      if (dedicated) {
        process.detection.integration_cpis = 1;
        process.detection.integration_motion_bins = 0;
      }
    }
    if (dedicated && process.performance?.acceleration === 'gpu-resident')
      process.performance.acceleration = 'auto';
    if (dedicated && process.tracker) {
      if (process.tracker.track_guided_shadow) {
        process.tracker.track_guided_shadow.enable = false;
        process.tracker.track_guided_shadow.promote = false;
      }
      if (process.tracker.initiate?.stationary_clutter)
        process.tracker.initiate.stationary_clutter.enable = false;
      if (process.tracker.initiate?.acceleration_model === 'adaptive_gate')
        process.tracker.initiate.acceleration_model = 'hypothesis_bank';
    }
  }
  function presetRecord(config, presetId) {
    const preset = layoutPresets().find(item => item.id === presetId);
    if (!preset) return null;
    const configuredCount = config?.capture?.device?.channel_count;
    if (configuredCount !== preset.channels) return null;
    const frequencyHz = Number(config?.capture?.fc);
    const spacing = frequencyHz > 0 ? lightSpeed / frequencyHz / 2 : .25;
    const bearingChannels = Array.from({length: preset.bearing}, (_, index) => index);
    let points;
    if (preset.shape === 'ula') {
      points = bearingChannels.map((_, index) =>
        [(index - (preset.bearing - 1) / 2) * spacing, 0, 0]);
    } else if (preset.shape === 'cross') {
      const radius = spacing / Math.sqrt(2);
      points = [[radius, 0, 0], [0, radius, 0],
        [-radius, 0, 0], [0, -radius, 0]];
    } else {
      const radius = spacing / (2 * Math.sin(Math.PI / preset.bearing));
      points = bearingChannels.map((_, index) => {
        const angle = 2 * Math.PI * index / preset.bearing;
        return [radius * Math.cos(angle), radius * Math.sin(angle), 0];
      });
    }
    const elements = bearingChannels.map((channel, index) => ({
      id: `element-${channel + 1}`, daq_channel: channel, position: points[index]
    }));
    if (preset.dedicated) {
      const reference = preset.channels - 1;
      const extent = Math.max(...points.map(point => Math.hypot(point[0], point[1])));
      elements.push({id: 'reference', daq_channel: reference,
        position: [0, -(extent + spacing), 0]});
    }
    return {preset, geometry: {
      shape: preset.shape, entry_template: preset.shape, units: 'm',
      coordinate_frame: 'array_xy_right_handed', mapping_confirmed: false,
      geometry_confirmed: false, orientation_confirmed: false,
      x_axis_bearing_deg_true: null, bearing_channels: bearingChannels,
      ula_half_plane: 'both', elements
    }};
  }
  function applyPreset(config, presetId) {
    const generated = presetRecord(config, presetId);
    if (!generated) return false;
    const device = config.capture.device;
    device.array_geometry = generated.geometry;
    if (!object(config.process)) config.process = {};
    const existingSynthesis = object(config.process.reference_synthesis) ?
      config.process.reference_synthesis : {};
    const channels = [...generated.geometry.bearing_channels];
    if (generated.preset.dedicated) {
      const reference = generated.preset.channels - 1;
      device.reference_channel = reference;
      device.surveillance_channels = channels;
      config.process.reference_synthesis = {...existingSynthesis,
        mode: 'dedicated', channels: [reference]};
      disableIncompatibleArrayFeatures(config, true);
    } else {
      device.reference_channel = channels[0];
      device.surveillance_channels = channels;
      config.process.reference_synthesis = {...existingSynthesis,
        mode: 'array_eigenbeam', channels};
      if (generated.preset.shape !== 'regular_polygon')
        disableIncompatibleArrayFeatures(config, false);
    }
    return true;
  }
  function channelRole(config, channel) {
    const device = config?.capture?.device || {};
    const synthesis = config?.process?.reference_synthesis || {};
    if (synthesis.mode === 'dedicated' && device.reference_channel === channel)
      return 'dedicated_reference';
    const surveillance = (device.surveillance_channels || []).includes(channel);
    const bearing = (device.array_geometry?.bearing_channels || []).includes(channel);
    if (surveillance && bearing) return 'array';
    if (surveillance) return 'surveillance';
    return 'disabled';
  }
  function applyChannelRole(config, channel, role) {
    const device = config.capture.device;
    const geometry = device.array_geometry;
    if (!object(config.process)) config.process = {};
    if (!object(config.process.reference_synthesis)) config.process.reference_synthesis = {};
    const synthesis = config.process.reference_synthesis;
    const count = device.channel_count;
    const valid = value => Number.isInteger(value) && value >= 0 && value < count;
    const add = (values, value) => [...new Set([...(values || []).filter(valid), value])].sort((a, b) => a - b);
    const remove = (values, value) => (values || []).filter(item => item !== value && valid(item));
    if (role === 'dedicated_reference') {
      const previous = device.reference_channel;
      device.reference_channel = channel;
      device.surveillance_channels = remove(device.surveillance_channels, channel);
      geometry.bearing_channels = remove(geometry.bearing_channels, channel);
      synthesis.mode = 'dedicated'; synthesis.channels = [channel];
      disableIncompatibleArrayFeatures(config, true);
      if (valid(previous) && previous !== channel && channelRole(config, previous) === 'disabled')
        device.surveillance_channels = add(device.surveillance_channels, previous);
    } else {
      if (synthesis.mode === 'dedicated' && device.reference_channel === channel) {
        synthesis.mode = 'array_eigenbeam';
        synthesis.channels = [...new Set([...(geometry.bearing_channels || []), channel])].filter(valid).sort((a, b) => a - b);
      }
      if (role === 'array') {
        device.surveillance_channels = add(device.surveillance_channels, channel);
        geometry.bearing_channels = add(geometry.bearing_channels, channel);
        if (synthesis.mode === 'array_eigenbeam') synthesis.channels = add(synthesis.channels, channel);
      } else if (role === 'surveillance') {
        device.surveillance_channels = add(device.surveillance_channels, channel);
        geometry.bearing_channels = remove(geometry.bearing_channels, channel);
        if (synthesis.mode === 'array_eigenbeam') synthesis.channels = remove(synthesis.channels, channel);
      } else if (role === 'disabled') {
        device.surveillance_channels = remove(device.surveillance_channels, channel);
        geometry.bearing_channels = remove(geometry.bearing_channels, channel);
        if (synthesis.mode === 'array_eigenbeam') synthesis.channels = remove(synthesis.channels, channel);
      }
    }
    invalidate(geometry);
    return config;
  }
  function applyReferenceMode(config, mode) {
    const device = config.capture.device;
    const geometry = device.array_geometry;
    const synthesis = config.process.reference_synthesis;
    if (mode === 'array_eigenbeam') {
      const oldReference = device.reference_channel;
      synthesis.mode = 'array_eigenbeam';
      device.surveillance_channels = [...new Set([...(device.surveillance_channels || []), oldReference])].sort((a, b) => a - b);
      geometry.bearing_channels = [...new Set([...(geometry.bearing_channels || []), oldReference])].sort((a, b) => a - b);
      const channels = [...new Set((geometry.bearing_channels || []).filter(channel =>
        device.surveillance_channels.includes(channel)))].sort((a, b) => a - b);
      synthesis.channels = channels.length >= 2 ? channels : Array.from({length: device.channel_count}, (_, index) => index);
    } else {
      let reference = device.reference_channel;
      if (!Number.isInteger(reference) || reference < 0 || reference >= device.channel_count)
        reference = device.channel_count - 1;
      device.reference_channel = reference;
      synthesis.mode = 'dedicated'; synthesis.channels = [reference];
      device.surveillance_channels = (device.surveillance_channels || []).filter(channel => channel !== reference);
      geometry.bearing_channels = (geometry.bearing_channels || []).filter(channel => channel !== reference);
      disableIncompatibleArrayFeatures(config, true);
    }
    invalidate(geometry); return config;
  }

  function channelRoleEditor(document, config, changed, canEdit) {
    const device = config.capture.device, geometry = device.array_geometry;
    const shell = document.createElement('section'); shell.className = 'geometry-channel-editor';
    const heading = document.createElement('div'); heading.className = 'geometry-channel-heading';
    heading.innerHTML = '<h3>Antennas and channel roles</h3><p>Select an antenna to edit it.</p>';
    const referenceLabel = document.createElement('label'); referenceLabel.className = 'geometry-reference-mode';
    const referenceTitle = document.createElement('span'); referenceTitle.textContent = 'Reference estimation';
    const referenceSelect = document.createElement('select');
    for (const [value, label] of [['array_eigenbeam', 'Automatic from array'], ['dedicated', 'Dedicated reference antenna']]) {
      const option = document.createElement('option'); option.value = value; option.textContent = label; referenceSelect.appendChild(option);
    }
    referenceSelect.value = config.process.reference_synthesis.mode;
    referenceSelect.disabled = !canEdit();
    referenceSelect.addEventListener('change', () => {
      applyReferenceMode(config, referenceSelect.value); changed(true);
    });
    referenceLabel.append(referenceTitle, referenceSelect); heading.appendChild(referenceLabel);
    const workspace = document.createElement('div'); workspace.className = 'geometry-channel-workspace';
    const map = document.createElement('div'); map.className = 'geometry-channel-map';
    const panel = document.createElement('div'); panel.className = 'geometry-channel-panel';
    let selected = geometry.elements.find(element => Number.isInteger(element?.daq_channel))?.daq_channel ?? 0;
    const positioned = geometry.elements.filter(element => Number.isInteger(element?.daq_channel));
    const extent = Math.max(.001, ...positioned.map(element => Math.hypot(Number(element.position?.[0]) || 0,
      Number(element.position?.[1]) || 0)));
    const renderPanel = () => {
      panel.replaceChildren();
      const element = geometry.elements.find(item => item?.daq_channel === selected);
      if (!element) { panel.textContent = 'Select a mapped antenna.'; return; }
      const title = document.createElement('h4'); title.textContent = `Channel ${selected} · ${element.id}`;
      const makeLabel = (text, control, help = '') => {
        const label = document.createElement('label'); const span = document.createElement('span'); span.textContent = text;
        label.append(span, control); if (help) { const small = document.createElement('small'); small.textContent = help; label.appendChild(small); }
        return label;
      };
      const role = document.createElement('select');
      for (const [value, label] of [['array', 'Array antenna + surveillance'], ['surveillance', 'Surveillance only'],
        ['dedicated_reference', 'Dedicated reference'], ['disabled', 'Disabled']]) {
        const option = document.createElement('option'); option.value = value; option.textContent = label; role.appendChild(option);
      }
      role.value = channelRole(config, selected); role.disabled = !canEdit();
      role.addEventListener('change', () => { applyChannelRole(config, selected, role.value); changed(true); });
      const labelInput = document.createElement('input'); labelInput.value = element.id || ''; labelInput.disabled = !canEdit();
      labelInput.addEventListener('input', () => { element.id = labelInput.value; invalidate(geometry); changed(false); });
      const coordinates = document.createElement('div'); coordinates.className = 'geometry-coordinate-grid';
      ['x (m)', 'y (m)', 'z (m)'].forEach((name, axis) => {
        const input = document.createElement('input'); input.type = 'number'; input.step = 'any';
        input.value = element.position?.[axis] ?? ''; input.disabled = !canEdit();
        input.addEventListener('input', () => {
          if (!Array.isArray(element.position)) element.position = [null, null, null];
          element.position[axis] = input.value === '' ? null : Number(input.value);
          invalidate(geometry); changed(false);
        }); coordinates.appendChild(makeLabel(name, input));
      });
      panel.append(title, makeLabel('Role', role, 'Array antennas participate in bearing; surveillance-only channels do not.'),
        makeLabel('Antenna label', labelInput), coordinates);
      const gains = config.capture.device.heimdall?.channel_gains;
      const automatic = config.capture.device.heimdall?.gain_control !== 'fixed';
      const gain = document.createElement('input'); gain.type = 'number'; gain.min = '0'; gain.max = '50'; gain.step = '.1';
      gain.value = Array.isArray(gains) && Number.isFinite(gains[selected]) ? gains[selected] : '';
      gain.disabled = automatic || !canEdit();
      gain.addEventListener('input', () => {
        const vector = Array.isArray(config.capture.device.heimdall.channel_gains) ?
          [...config.capture.device.heimdall.channel_gains] : Array.from({length: device.channel_count}, () => 0);
        vector[selected] = Number(gain.value); config.capture.device.heimdall.channel_gains = vector; changed(false);
      });
      panel.appendChild(makeLabel(automatic ? 'Gain selected automatically' : 'Manual gain (dB)', gain,
        automatic ? 'Change Gain to Manual to edit this channel.' : 'Applied to this tuner after save and restart.'));
    };
    positioned.forEach(element => {
      const channel = element.daq_channel;
      const button = document.createElement('button'); button.type = 'button';
      button.className = `geometry-channel-node role-${channelRole(config, channel)}`;
      const x = Number(element.position?.[0]) || 0, y = Number(element.position?.[1]) || 0;
      button.style.left = `${50 + x / extent * 35}%`; button.style.top = `${50 - y / extent * 35}%`;
      button.innerHTML = `<strong>CH${channel}</strong><small>${channelRole(config, channel).replaceAll('_', ' ')}</small>`;
      button.addEventListener('click', () => {
        selected = channel; map.querySelectorAll('button').forEach(item => item.classList.toggle('selected', item === button)); renderPanel();
      });
      if (channel === selected) button.classList.add('selected'); map.appendChild(button);
    });
    renderPanel(); workspace.append(map, panel); shell.append(heading, workspace); return shell;
  }
  function presetGallery(document, config, choose, editable) {
    const shell = document.createElement('section'); shell.className = 'geometry-presets';
    const title = document.createElement('h3'); title.textContent = 'Common array layouts';
    const explanation = document.createElement('p');
    explanation.textContent = 'Choose a layout.';
    const countPicker = document.createElement('label'); countPicker.className = 'geometry-preset-counts';
    const countTitle = document.createElement('span'); countTitle.textContent = 'Antennas connected';
    const countSelect = document.createElement('select');
    for (let count = 2; count <= 8; count++) {
      const option = document.createElement('option'); option.value = String(count);
      option.textContent = `${count} antennas`; countSelect.appendChild(option);
    }
    countSelect.value = String(config.capture.device.channel_count);
    countSelect.disabled = !editable;
    countPicker.append(countTitle, countSelect);
    const grid = document.createElement('div'); grid.className = 'geometry-preset-grid';
    let previewCount = config.capture.device.channel_count;
    const renderCards = () => {
      grid.replaceChildren();
      layoutPresets().filter(preset => preset.channels === previewCount).forEach(preset => {
      const card = document.createElement('article'); card.className = 'geometry-preset-card';
      const heading = document.createElement('h4'); heading.textContent = preset.title;
      const diagram = document.createElement('div'); diagram.className = `geometry-preset-diagram ${preset.shape}`;
      for (let index = 0; index < preset.bearing; index++) {
        const node = document.createElement('i');
        let x, y;
        if (preset.shape === 'ula') {
          x = 18 + 64 * index / (preset.bearing - 1); y = 50;
        } else if (preset.shape === 'cross') {
          [[72, 50], [28, 50], [50, 23], [50, 77]][index].forEach((value, axis) => {
            if (axis === 0) x = value; else y = value;
          });
        } else {
          const angle = 2 * Math.PI * index / preset.bearing;
          x = 50 + Math.cos(angle) * 29; y = 50 - Math.sin(angle) * 29;
        }
        node.style.left = `${x}%`; node.style.top = `${y}%`; diagram.appendChild(node);
      }
      if (preset.dedicated) { const reference = document.createElement('i'); reference.className = 'reference'; diagram.appendChild(reference); }
      const detail = document.createElement('p');
      detail.textContent = `${preset.bearing} bearing element${preset.bearing === 1 ? '' : 's'}${preset.dedicated ? ' + 1 dedicated reference' : ' using array synthesis'}`;
      const action = document.createElement('button'); action.type = 'button';
      action.disabled = !editable;
      action.textContent = 'Use this layout';
      action.addEventListener('click', () => { if (!action.disabled) choose(preset); });
      card.append(heading, diagram, detail, action); grid.appendChild(card);
      });
    };
    countSelect.addEventListener('change', () => {
      previewCount = Number(countSelect.value); renderCards();
    });
    renderCards();
    shell.append(title, explanation, countPicker, grid); return shell;
  }
  function invalidate(geometry) {
    if (object(geometry)) confirmations.forEach(key => { geometry[key] = false; });
    return geometry;
  }
  function context(config) {
    if (!config) return null;
    const device = config.capture?.device || {};
    const geometry = {...device.array_geometry};
    confirmations.forEach(key => { delete geometry[key]; });
    const heimdall = object(device.heimdall) ? {...device.heimdall} : device.heimdall;
    if (object(heimdall)) {
      delete heimdall.gain;
      delete heimdall.gain_control;
      delete heimdall.channel_gains;
    }
    return JSON.stringify([device.type,
      device.channel_count, device.reference_channel, device.surveillance_channels,
      heimdall, config.process?.reference_synthesis, geometry]);
  }
  function reconcile(config, previous) {
    if (previous && context(config) !== context(previous))
      invalidate(config.capture?.device?.array_geometry);
    return config;
  }
  function recordErrors(geometry) {
    if (geometry === undefined) return [];
    const errors = [];
    const fail = (key, message) => errors.push(`${prefix}${key ? '.' + key : ''}: ${message}`);
    let nodes = 0;
    const visit = (value, depth = 0) => {
      if (++nodes > 512 || depth > 6) throw new Error('Record is too large or deeply nested.');
      if (typeof value === 'string' && value.length > 1024) throw new Error('Metadata strings must be at most 1024 characters.');
      if (typeof value === 'number' && !finite(value)) throw new Error('Numbers must be finite.');
      if (value && typeof value === 'object') {
        if (Array.isArray(value) && value.length > 32) throw new Error('Metadata lists must be at most 32 entries.');
        for (const [key, child] of Object.entries(value)) {
          if (['__proto__', 'prototype', 'constructor'].includes(key)) throw new Error('Unsafe metadata key.');
          visit(child, depth + 1);
        }
      }
    };
    try { visit(geometry); } catch (error) { fail('', error.message); return errors; }
    if (!object(geometry)) { fail('', 'Must be an object.'); return errors; }
    if (!shapes.includes(geometry.shape)) fail('shape', 'Choose custom, cross, regular_polygon or ula.');
    if (geometry.entry_template !== undefined && ![...shapes, 'pentagon'].includes(geometry.entry_template)) fail('entry_template', 'Choose one of the layout guides.');
    if (geometry.units !== 'm') fail('units', 'Positions must use metres (m).');
    if (geometry.coordinate_frame !== 'array_xy_right_handed') fail('coordinate_frame', 'Use array_xy_right_handed.');
    confirmations.forEach(key => {
      if (geometry[key] !== undefined && typeof geometry[key] !== 'boolean') fail(key, 'Must be true or false.');
    });
    const angle = geometry.x_axis_bearing_deg_true;
    if (angle != null && (!finite(angle) || angle < 0 || angle >= 360)) fail('x_axis_bearing_deg_true', 'Use degrees from 0 up to, but not including, 360.');
    if (geometry.ula_half_plane !== undefined && !['both', 'positive_y', 'negative_y'].includes(geometry.ula_half_plane)) fail('ula_half_plane', 'Choose both, positive_y or negative_y.');
    const channels = geometry.bearing_channels;
    if (!Array.isArray(channels) || channels.length > 8 || Array.from(channels || []).some(value => !Number.isInteger(value) || value < 0 || value > 7) || new Set(channels).size !== channels.length)
      fail('bearing_channels', 'Use up to eight unique DAQ channels from 0 through 7.');
    const elements = geometry.elements;
    if (!Array.isArray(elements) || elements.length > 8) { fail('elements', 'Use up to eight measured elements.'); return errors; }
    const ids = new Set(), mapped = new Set(), serials = new Set();
    for (let index = 0; index < elements.length; index++) {
      const item = elements[index], key = `elements.${index}`;
      if (!object(item)) { fail(key, 'Must be an element object.'); continue; }
      for (const name of ['id', 'receiver_serial']) {
        const value = item[name];
        if (value === undefined && name === 'receiver_serial') continue;
        if (typeof value !== 'string' || !value.trim() || value !== value.trim() || value.length > 128 || /[\u0000-\u001f\u007f]/.test(value)) fail(`${key}.${name}`, 'Use a trimmed label of 1–128 characters.');
        const seen = name === 'id' ? ids : serials;
        if (seen.has(value)) fail(`${key}.${name}`, 'Must be unique.');
        seen.add(value);
      }
      const channel = item.daq_channel;
      if (channel !== null && (!Number.isInteger(channel) || channel < 0 || channel > 7)) fail(`${key}.daq_channel`, 'Choose DAQ 0 through 7, or leave unassigned.');
      if (channel !== null && mapped.has(channel)) fail(`${key}.daq_channel`, 'Each DAQ channel may be assigned once.');
      mapped.add(channel);
      if (!Array.isArray(item.position) || item.position.length !== 3 ||
          [0, 1, 2].some(axis => item.position[axis] !== null && (!finite(item.position[axis]) || Math.abs(item.position[axis]) > Number.MAX_SAFE_INTEGER)))
        fail(`${key}.position`, 'Enter x, y and z in metres, or leave missing coordinates blank.');
    }
    return errors;
  }
  function emptyRecord(count) {
    return {shape: 'custom', units: 'm', coordinate_frame: 'array_xy_right_handed',
      mapping_confirmed: false, geometry_confirmed: false, orientation_confirmed: false,
      x_axis_bearing_deg_true: null, bearing_channels: [], ula_half_plane: 'both',
      elements: Array.from({length: Math.max(2, Math.min(8, count || 2))}, (_, index) =>
        ({id: `element-${index + 1}`, daq_channel: null, position: [null, null, null]}))};
  }
  function render(document, config, changed, editable = true, resizeChannels = null) {
    const canEdit = typeof editable === 'function' ? editable : () => editable;
    const device = config.capture.device;
    const section = document.createElement('details');
    section.className = 'config-group kraken-geometry';
    section.dataset.configGroup = prefix;
    const heading = document.createElement('summary');
    heading.textContent = device.type === 'Kraken' ? 'Array setup' : 'Saved Kraken array setup (inactive)';
    section.appendChild(heading);
    const content = document.createElement('div');
    content.className = 'config-fields';
    section.appendChild(content);
    const note = text => { const p = document.createElement('p'); p.textContent = text; content.appendChild(p); return p; };
    note(config.process?.bearing?.enable === true ? 'Bearing output on.' : 'Bearing output off.');
    const geometry = device.array_geometry;
    const button = (text, action) => {
      const b = document.createElement('button'); b.type = 'button'; b.textContent = text;
      b.disabled = !canEdit(); b.addEventListener('click', () => { if (canEdit()) action(); }); return b;
    };
    if (geometry === undefined) {
      note('Unknown geometry does not prevent passive radar.');
      content.appendChild(button('Add antenna layout', () => { device.array_geometry = emptyRecord(device.channel_count); changed(true); }));
      return section;
    }
    content.appendChild(button('Remove layout', () => {
      if (document.defaultView?.confirm('Remove this layout and its measurements from the draft? Discard changes can restore them until you save.')) {
        delete device.array_geometry;
        changed(true);
      }
    }));
    if (device.type !== 'Kraken') {
      note('Kept for Kraken. Select Kraken to edit, or remove it from the draft.');
      return section;
    }
    const visualHost = document.createElement('div');
    visualHost.className = 'geometry-visual-host';
    content.appendChild(visualHost);
    const drawVisual = () => visualHost.replaceChildren(comparisonVisual(document, config));
    drawVisual();
    const assessment = document.createElement('p'); assessment.id = 'geometry-assessment';
    assessment.setAttribute('aria-live', 'polite'); content.appendChild(assessment);
    if (!object(geometry) || !Array.isArray(geometry.elements) || geometry.elements.length > 8) {
      note('Unsupported saved layout. Its values are kept; remove it to start again.');
      return section;
    }
    const update = (action, rebuild = false, attestation = false) => {
      if (!canEdit()) return;
      if (!attestation) invalidate(geometry);
      action();
      drawVisual();
      if (!attestation) section.querySelectorAll('[data-attestation]').forEach(input => { input.checked = false; });
      changed(rebuild);
    };
    const presetHost = document.createElement('div');
    presetHost.className = 'geometry-preset-host';
    presetHost.appendChild(presetGallery(document, config, preset => {
      if (document.defaultView?.confirm(`Replace the unsaved layout with ${preset.title}? You must measure and confirm the installed positions before saving.`)) {
        const previousCount = device.channel_count;
        device.channel_count = preset.channels;
        if (typeof resizeChannels === 'function') resizeChannels(preset.channels, previousCount);
        applyPreset(config, preset.id);
        changed(true);
      }
    }, canEdit()));
    content.insertBefore(presetHost, assessment);
    content.insertBefore(channelRoleEditor(document, config, changed, canEdit), assessment);
    const manualDetails = document.createElement('details');
    manualDetails.className = 'geometry-manual';
    const manualSummary = document.createElement('summary');
    manualSummary.textContent = 'Measured layout and channel mapping';
    const manualBody = document.createElement('div');
    manualBody.className = 'config-fields';
    manualDetails.append(manualSummary, manualBody);
    content.insertBefore(manualDetails, assessment);
    function field(parent, key, title, help, control) {
      const wrapper = document.createElement('div'); wrapper.className = 'config-field'; wrapper.dataset.path = `${prefix}.${key}`;
      const label = document.createElement('label'); control.id = `geometry-${key.replace(/\./g, '-')}`; label.htmlFor = control.id; label.textContent = title;
      const small = document.createElement('small'); small.textContent = help;
      const error = document.createElement('span'); error.className = 'config-field-error';
      control.disabled = !canEdit(); control.dataset.originalDisabled = String(!canEdit());
      wrapper.append(label, control, small, error); parent.appendChild(wrapper); return control;
    }
    function select(values, value, action) {
      const control = document.createElement('select');
      for (const [key, title] of values) { const option = document.createElement('option'); option.value = key; option.textContent = title; control.appendChild(option); }
      const selected = value == null ? '' : ['string', 'number'].includes(typeof value) ? String(value) : '__unsupported_saved_value__';
      if (![...control.options].some(option => option.value === selected)) {
        const option = document.createElement('option'); option.value = selected;
        option.textContent = `Unsupported saved value: ${typeof value === 'string' ? value : JSON.stringify(value)}`;
        control.appendChild(option); control.dataset.unsupported = 'true';
        control.setCustomValidity('Choose a supported value; the saved value has been retained.');
      }
      control.value = selected;
      control.addEventListener('change', () => update(() => action(control.value), true)); return control;
    }
    function input(value, action, type = 'text') {
      const control = document.createElement('input'); control.type = type; control.value = value ?? '';
      if (type === 'number') { control.step = 'any'; control.min = -Number.MAX_SAFE_INTEGER; control.max = Number.MAX_SAFE_INTEGER; }
      else control.maxLength = 128;
      control.addEventListener('input', () => update(() => action(type === 'number' ? control.value === '' ? null : Number(control.value) : control.value))); return control;
    }
    const template = select([['custom', 'Measured custom layout'], ['pentagon', 'Five physical pentagon vertices'],
      ['regular_polygon', 'Regular polygon / UCA bearing subset'], ['ula', 'Uniform linear array'], ['cross', 'Four + surveillance arms and separate reference']], geometry.entry_template ?? geometry.shape, value => {
      geometry.entry_template = value;
      geometry.shape = value === 'pentagon' ? (device.surveillance_channels?.length === 5 && config.process?.reference_synthesis?.mode !== 'dedicated' ? 'regular_polygon' : 'custom') : value;
    });
    field(content, 'entry_template', 'Layout guide', 'Guides entry; keeps all measurements and channel roles.', template);
    if (['pentagon', 'regular_polygon'].includes(geometry.entry_template))
      note('A dedicated reference uses one channel outside the bearing array.');
    field(content, 'shape', 'Bearing subset shape', 'Classifies selected bearing elements only.', select(shapes.map(value => [value, {custom: 'Custom', cross: 'Four-arm cross', regular_polygon: 'Regular polygon', ula: 'Uniform line'}[value]]), geometry.shape, value => { geometry.shape = value; }));
    note('Measure x/y/z in metres and match each DAQ channel.');
    const table = document.createElement('div'); table.className = 'geometry-elements'; content.appendChild(table);
    geometry.elements.forEach((element, index) => {
      if (!object(element)) { note(`Element ${index + 1} needs repair in the saved file.`); return; }
      const row = document.createElement('fieldset'); row.className = 'geometry-element'; table.appendChild(row);
      const legend = document.createElement('legend'); legend.textContent = `Element ${index + 1}`; row.appendChild(legend);
      const key = `elements.${index}`;
      field(row, `${key}.id`, 'Element label', 'Your physical antenna label.', input(element.id, value => { element.id = value; }));
      field(row, `${key}.daq_channel`, 'DAQ channel', 'Cable mapping, not USB order.', select([['', 'Unassigned'], ...Array.from({length: 8}, (_, channel) => [String(channel), `DAQ ${channel}${channel >= device.channel_count ? ' (inactive)' : ''}`])], element.daq_channel, value => { element.daq_channel = value === '' ? null : Number(value); }));
      field(row, `${key}.receiver_serial`, 'Receiver serial', 'Optional.', input(element.receiver_serial, value => { if (value === '') delete element.receiver_serial; else element.receiver_serial = value; }));
      for (const [axis, label] of ['x', 'y', 'z'].entries()) {
        field(row, `${key}.position.${axis}`, `${label} (m)`, 'Blank means unknown.', input(element.position?.[axis], value => {
          if (!Array.isArray(element.position)) element.position = [null, null, null];
          element.position[axis] = value;
        }, 'number'));
      }
      const channel = element.daq_channel;
      const dedicated = config.process?.reference_synthesis?.mode === 'dedicated';
      const active = Number.isInteger(channel) && channel < device.channel_count;
      const surveillance = active && device.surveillance_channels?.includes(channel);
      const ref = active && (dedicated ? device.reference_channel === channel : config.process?.reference_synthesis?.channels?.includes(channel));
      const role = document.createElement('p'); role.className = 'geometry-role';
      role.textContent = !active ? 'Role: unassigned or inactive' : `Role: ${[surveillance ? 'surveillance' : '', ref ? dedicated ? 'dedicated reference' : 'reference synthesis' : ''].filter(Boolean).join(' + ') || 'unused'}`;
      row.appendChild(role);
      const bearing = document.createElement('input'); bearing.type = 'checkbox'; bearing.checked = Array.isArray(geometry.bearing_channels) && geometry.bearing_channels.includes(channel);
      bearing.addEventListener('change', () => update(() => {
        const old = Array.isArray(geometry.bearing_channels) ? geometry.bearing_channels : [];
        geometry.bearing_channels = bearing.checked ? [...old.filter(value => value !== channel), channel] : old.filter(value => value !== channel);
      }));
      field(row, `${key}.bearing`, 'Bearing subset', '', bearing);
      if (!surveillance || (dedicated && ref)) { bearing.disabled = true; bearing.dataset.originalDisabled = 'true'; }
      if (bearing.checked && bearing.disabled) { const warning = document.createElement('p'); warning.textContent = 'Saved bearing selection conflicts with the current role; review the subset.'; row.appendChild(warning); }
      row.appendChild(button(`Remove element ${index + 1}`, () => update(() => {
        geometry.elements.splice(index, 1);
        geometry.bearing_channels = (Array.isArray(geometry.bearing_channels) ? geometry.bearing_channels : []).filter(value => value !== channel);
      }, true)));
    });
    if (geometry.elements.length < 8) content.appendChild(button('Add measured element', () => update(() => {
      const ids = new Set(geometry.elements.map(item => item?.id)); let index = 1; while (ids.has(`element-${index}`)) index++;
      geometry.elements.push({id: `element-${index}`, daq_channel: null, position: [null, null, null]});
    }, true)));
    // Explicitly clear the subset without dropping any physical measurements.
    content.appendChild(button('Clear bearing selection', () => update(() => { geometry.bearing_channels = []; }, true)));
    const angle = input(geometry.x_axis_bearing_deg_true, value => { geometry.x_axis_bearing_deg_true = value; }, 'number'); angle.min = 0; angle.max = 359.999999999;
    field(content, 'x_axis_bearing_deg_true', '+x bearing from true north (degrees)', 'Clockwise: 0 north, 90 east. Leave unknown orientation blank.', angle);
    field(content, 'ula_half_plane', 'Linear-array half-plane', 'Both retains front/back ambiguity.', select([['both', 'Both (ambiguous)'], ['positive_y', 'Local +y only'], ['negative_y', 'Local −y only']], geometry.ula_half_plane ?? 'both', value => { geometry.ula_half_plane = value; }));
    note('Save before confirming measurements.');
    for (const [key, title] of [['mapping_confirmed', 'I checked the cable map'], ['geometry_confirmed', 'I measured the element positions'], ['orientation_confirmed', 'I surveyed true-north orientation']]) {
      const control = document.createElement('input'); control.type = 'checkbox'; control.checked = geometry[key] === true; control.dataset.attestation = key;
      control.addEventListener('change', () => update(() => { geometry[key] = control.checked; }, false, true));
      field(content, key, title, '', control);
    }
    while (assessment.nextSibling) manualBody.appendChild(assessment.nextSibling);
    return section;
  }
  function showAssessment(document, assessment) {
    const target = document.getElementById('geometry-assessment');
    if (!target || !assessment) return;
    const errors = assessment.issues?.filter(issue => issue.severity === 'error') || [];
    target.textContent = assessment.valid ? 'Measurements not confirmed.' :
      `Layout needs review: ${errors.slice(0, 3).map(issue => issue.message).join(' ')}${errors.length > 3 ? ' Review additional measurements or mappings.' : ''}`;
  }
  return {recordErrors, emptyRecord, invalidate, context, reconcile, layoutComparison,
    layoutPresets, presetRecord, applyPreset, channelRole, applyChannelRole,
    applyReferenceMode, render, showAssessment};
});
