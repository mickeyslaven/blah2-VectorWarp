'use strict';

const {siteCatalog} = require('./multi-illuminator-track-fusion');
const finite = Number.isFinite;
const norm = values => Math.hypot(...values);
const sub = (a,b) => a.map((x,i)=>x-b[i]);
const median = values => {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b), middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};
const validLocation = value => [value?.latitude, value?.longitude, value?.altitude]
  .every(item => finite(item)) && Math.abs(Number(value.latitude)) <= 90 &&
  Math.abs(Number(value.longitude)) <= 180;

function gradient(point, transmitter) {
  const fromTransmitter = sub(point, transmitter), receiverRange = norm(point);
  const transmitterRange = norm(fromTransmitter);
  if (!(receiverRange > 1e-6) || !(transmitterRange > 1e-6)) return null;
  return point.map((value, index) => value / receiverRange +
    fromTransmitter[index] / transmitterRange);
}

function nearest(values, target) {
  if (!Array.isArray(values) || !values.length || !finite(target) ||
      target < values[0] || target > values[values.length - 1]) return -1;
  let low = 0, high = values.length - 1;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (Number(values[middle]) < target) low = middle + 1; else high = middle;
  }
  if (low > 0 && Math.abs(Number(values[low - 1]) - target) <=
      Math.abs(Number(values[low]) - target)) return low - 1;
  return low;
}

// Published frame maps are immutable. Validate axes once, not per hypothesis.
const axisValidity = new WeakMap();
function monotonic(axis) {
  if (!Array.isArray(axis)) return false;
  if (!axisValidity.has(axis)) axisValidity.set(axis, axis.length > 0 &&
    axis.every((value, index) => finite(value) && (!index || value > axis[index - 1])));
  return axisValidity.get(axis);
}
function sampleMap(map, delayKm, dopplerHz, radius = 4) {
  if (!monotonic(map?.delay) || !monotonic(map?.doppler)) return null;
  const row = nearest(map?.doppler, dopplerHz), column = nearest(map?.delay, delayKm);
  const data = map?.data;
  if (row < 0 || column < 0 || !Array.isArray(data) || data.length !== map.doppler.length || !Array.isArray(data[row]) ||
      !finite(data[row][column])) return null;
  const background = [];
  for (let y = Math.max(0, row - radius); y <= Math.min(data.length - 1, row + radius); y += 1) {
    if (!Array.isArray(data[y]) || data[y].length !== map.delay.length) return null;
    for (let x = Math.max(0, column - radius); x <= Math.min(data[y].length - 1, column + radius); x += 1) {
      if (Math.abs(y - row) <= 1 && Math.abs(x - column) <= 1) continue;
      if (finite(data[y][x])) background.push(data[y][x]);
    }
  }
  if (background.length < 16) return null;
  const centre = Number(data[row][column]), baseline = median(background);
  const mad = median(background.map(value => Math.abs(value - baseline)));
  const sigma = Math.max(.25, 1.4826 * mad);
  const residualDb = centre - baseline;
  const empiricalP = (1 + background.filter(value => value >= centre).length) /
    (background.length + 1);
  return {delay_km: delayKm, doppler_hz: dopplerHz, row, column,
    value_db: centre, local_median_db: baseline, local_mad_db: mad,
    residual_db: residualDb, robust_z: residualDb / sigma, empirical_upper_tail_p: empiricalP,
    positive_excess_linear: Math.max(0, 10 ** (residualDb / 10) - 1),
    local_background_cells: background.length};
}

function enabledDefinitions(config) {
  return (config?.process?.multi_illuminator?.illuminators || [])
    .filter(item => item?.enabled === true && validLocation(item.transmitter));
}

function siteGroups(config) {
  const receiver = config?.location?.rx;
  if (!validLocation(receiver)) return [];
  const multi = config?.process?.multi_illuminator || {};
  const radiusKm = finite(multi.co_location_radius_m) && multi.co_location_radius_m >= 0 ?
    multi.co_location_radius_m / 1000 : (multi.localization?.siteSeparationKm ?? .1);
  const definitions = enabledDefinitions(config);
  const catalog = siteCatalog(definitions, receiver, radiusKm, multi.transmitter_site_grouping);
  const groups = new Map();
  for (const definition of definitions) {
    const site = catalog.get(definition.id);
    if (!site || site.invalid) return [];
    if (!groups.has(site.identity)) groups.set(site.identity, {id: site.identity,
      definitions: [], transmitter: site.point, sites: new Map()});
    const group = groups.get(site.identity);
    group.definitions.push(definition);
    group.sites.set(definition.id, site);
  }
  return [...groups.values()];
}

module.exports = {gradient, sampleMap, siteGroups, enabledDefinitions};
