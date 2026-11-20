'use strict';
// Regression purpose: Cartesian Plotly consumers retain exact layouts and isolated mutable state.
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const root = path.join(__dirname, '../..');
const commonPath = path.join(root, 'html/js/common.js');
const plain = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));

function loadTheme(source = fs.readFileSync(commonPath, 'utf8')) {
  const module = {exports: {}};
  vm.runInNewContext(source, {module, exports: module.exports, URL, URLSearchParams,
    setTimeout, clearTimeout, console}, {filename: 'common.js'});
  return module.exports.vectorwarpPlotTheme;
}

function expectedAxis(text, y = false) {
  return {gridcolor: '#47362e', zerolinecolor: '#47362e', title: {text, font: {size: 18}},
    showgrid: false, ticks: '', ...(y ? {ticksuffix: ' ', categoryorder: 'total descending'} : {side: 'bottom'})};
}

const expectedBase = revision => ({font: {color: '#f3eee9'}, autosize: true,
  margin: {l: 92, r: 50, b: 50, t: 10, pad: 0}, hoverlabel: {namelength: 0},
  plot_bgcolor: 'rgba(0,0,0,0)', paper_bgcolor: 'rgba(0,0,0,0)', annotations: [],
  uirevision: revision, displayModeBar: false});
const expectedLegend = {orientation: 'h', bgcolor: '#29201c', bordercolor: '#47362e', borderwidth: 1};

function verify(layout, {revision, xTitle, yTitle, legend}) {
  assert.deepStrictEqual(plain({...layout, xaxis: undefined, yaxis: undefined, legend: undefined}),
    expectedBase(revision));
  assert.deepStrictEqual(plain(layout.xaxis), expectedAxis(xTitle));
  assert.deepStrictEqual(plain(layout.yaxis), expectedAxis(yTitle, true));
  assert.deepStrictEqual(plain(layout.legend), legend ? expectedLegend : undefined);
}

function consumerLayout(file, theme, xTitle, yTitle) {
  const calls = [];
  const context = vm.createContext({VectorWarpPlotTheme: theme, xTitle, yTitle,
    xVariable: 'timestamp', yVariable: 'doppler', Plotly: {react: (...args) => calls.push(args)},
    startRadarDisplay: () => Promise.resolve(), radarDisplayUrl: () => '/api/detection',
    radarVisualSemantics: {labels: {detection: 'Detection'}, legendRank: {detection: 1},
      colors: {detection: '#fff'}}, window: undefined});
  vm.runInContext(fs.readFileSync(path.join(root, 'html/js', file), 'utf8'), context, {filename: file});
  assert.equal(calls.length, 1, `${file} initializes one Plotly root`);
  return calls[0][2];
}

function verifyConsumers(theme) {
  for (const [xTitle, yTitle] of [
    ['Timestamp', 'Bistatic Range (km)'],
    ['Timestamp', 'Bistatic Doppler (Hz)'],
    ['Bistatic Range (km)', 'Bistatic Doppler (Hz)']
  ]) verify(consumerLayout('plot_detection.js', theme, xTitle, yTitle),
    {revision: 'vectorwarp-detections', xTitle, yTitle, legend: false});
  verify(consumerLayout('plot_timing.js', theme, 'Timestamp', 'Stage time (ms)'),
    {revision: 'vectorwarp-timing', xTitle: 'Timestamp', yTitle: 'Stage time (ms)', legend: true});
}

function assertFresh(theme) {
  const first = theme.cartesianLayout('first', 'First X', 'First Y', {legend: true});
  const second = theme.cartesianLayout('second', 'Second X', 'Second Y', {legend: true});
  for (const key of ['font', 'margin', 'hoverlabel', 'annotations', 'xaxis', 'yaxis', 'legend'])
    assert.notStrictEqual(first[key], second[key], `${key} is fresh per layout`);
  for (const axis of ['xaxis', 'yaxis']) {
    assert.notStrictEqual(first[axis].title, second[axis].title, `${axis} titles are fresh`);
    assert.notStrictEqual(first[axis].title.font, second[axis].title.font, `${axis} title fonts are fresh`);
  }
  first.font.color = '#000'; first.margin.l = 0; first.hoverlabel.namelength = 9; first.annotations.push({});
  first.xaxis.gridcolor = '#000'; first.xaxis.title.font.size = 1;
  first.yaxis.zerolinecolor = '#000'; first.yaxis.title.font.size = 1; first.legend.bgcolor = '#000';
  verify(second, {revision: 'second', xTitle: 'Second X', yTitle: 'Second Y', legend: true});
  verify(theme.cartesianLayout('third', 'Third X', 'Third Y', {legend: true}),
    {revision: 'third', xTitle: 'Third X', yTitle: 'Third Y', legend: true});
}

const production = fs.readFileSync(commonPath, 'utf8');
const theme = loadTheme(production);
verifyConsumers(theme);
assertFresh(theme);

const mutants = {
  hardcoded_axes: production.replaceAll('text: xTitle', "text: 'Delay (km)'")
    .replaceAll('text: yTitle', "text: 'Doppler (Hz)'"),
  wrong_axis_colors: production.replaceAll("'#47362e'", "'#ffffff'"),
  shared_font: production.replace('const vectorwarpPlotTheme', 'const sharedFontForMutation = {size: 18};\nconst vectorwarpPlotTheme')
    .replaceAll('font: {size: 18}', 'font: sharedFontForMutation')
};
for (const [name, source] of Object.entries(mutants)) assert.throws(() => {
  const mutant = loadTheme(source);
  verifyConsumers(mutant);
  assertFresh(mutant);
}, assert.AssertionError, `${name} must be killed by the focused production contract`);

for (const [file, invocation] of [
  ['plot_detection.js', /cartesianLayout\('vectorwarp-detections', xTitle, yTitle\)/],
  ['plot_timing.js', /cartesianLayout\('vectorwarp-timing', xTitle, yTitle, \{legend: true\}\)/]
]) {
  const source = fs.readFileSync(path.join(root, 'html/js', file), 'utf8');
  assert.match(source, invocation, `${file} uses the canonical Cartesian shell`);
  assert.doesNotMatch(source, /gridcolor: '#47362e'/, `${file} retains no duplicate Cartesian shell`);
}
console.log('Cartesian consumers preserve real titles, complete axes, legend, zoom revision, and fresh nested state.');
