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
  function invalidate(geometry) {
    if (object(geometry)) confirmations.forEach(key => { geometry[key] = false; });
    return geometry;
  }
  function context(config) {
    if (!config) return null;
    const device = config.capture?.device || {};
    const geometry = {...device.array_geometry};
    confirmations.forEach(key => { delete geometry[key]; });
    return JSON.stringify([config.capture?.fc, config.capture?.fs, config.capture?.replay, device.type,
      device.channel_count, device.reference_channel, device.surveillance_channels,
      device.heimdall, config.process?.reference_synthesis, geometry]);
  }
  function reconcile(config, previous) {
    if (previous && context(config) !== context(previous)) invalidate(config.capture?.device?.array_geometry);
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
  function render(document, config, changed, editable = true) {
    const canEdit = typeof editable === 'function' ? editable : () => editable;
    const device = config.capture.device;
    const section = document.createElement('details');
    section.className = 'config-group kraken-geometry';
    section.dataset.configGroup = prefix;
    const heading = document.createElement('summary');
    heading.textContent = device.type === 'Kraken' ? 'Antenna layout (optional)' : 'Saved Kraken layout (inactive)';
    section.appendChild(heading);
    const content = document.createElement('div');
    content.className = 'config-fields';
    section.appendChild(content);
    const note = text => { const p = document.createElement('p'); p.textContent = text; content.appendChild(p); return p; };
    note('Saved layout only; measurements are unverified. Bearing is unavailable. Not sent to Suite.');
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
    section.open = true;
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
      if (!attestation) section.querySelectorAll('[data-attestation]').forEach(input => { input.checked = false; });
      changed(rebuild);
    };
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
      note('Five pentagon vertices with one reference leave four bearing elements (custom). Five bearing elements plus a separate reference need six channels.');
    field(content, 'shape', 'Bearing subset shape', 'Classifies selected bearing elements only.', select(shapes.map(value => [value, {custom: 'Custom', cross: 'Four-arm cross', regular_polygon: 'Regular polygon', ula: 'Uniform line'}[value]]), geometry.shape, value => { geometry.shape = value; }));
    note('Measure positions in metres: +z up, +x/+y right-handed. Trace each cable to assign its DAQ channel. Live serial order and survey freshness are unknown.');
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
    note('Save layout changes before confirming these survey details. Changing the layout clears earlier confirmations.');
    for (const [key, title] of [['mapping_confirmed', 'I checked the cable map'], ['geometry_confirmed', 'I measured the element positions'], ['orientation_confirmed', 'I surveyed true-north orientation']]) {
      const control = document.createElement('input'); control.type = 'checkbox'; control.checked = geometry[key] === true; control.dataset.attestation = key;
      control.addEventListener('change', () => update(() => { geometry[key] = control.checked; }, false, true));
      field(content, key, title, '', control);
    }
    return section;
  }
  function showAssessment(document, assessment) {
    const target = document.getElementById('geometry-assessment');
    if (!target || !assessment) return;
    const errors = assessment.issues?.filter(issue => issue.severity === 'error') || [];
    target.textContent = assessment.valid ? 'Layout complete; measurements are unverified.' :
      `Layout needs review: ${errors.slice(0, 3).map(issue => issue.message).join(' ')}${errors.length > 3 ? ' Review additional measurements or mappings.' : ''}`;
  }
  return {recordErrors, emptyRecord, invalidate, context, reconcile, render, showAssessment};
});
