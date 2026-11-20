'use strict';

// Shared inventory for the CI DOM audit and real-browser layout runner. New
// pages and literal navigation destinations automatically enter both audits.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '../../html');
function filesBelow(directory) {
  return fs.readdirSync(directory, {withFileTypes: true}).flatMap(entry => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? filesBelow(file) : [file];
  });
}
const normalizeRoute = value => {
  const url = new URL(value, 'http://fixture');
  return url.pathname.replace(/(?:index\.html|\/$)/, '') + (url.pathname === '/' ? '/' : '') + url.search;
};
function routeInventory() {
  const files = filesBelow(root);
  const pages = files.filter(file => path.basename(file) === 'index.html');
  const aliases = [], routes = new Set(), links = new Set();
  for (const file of pages) {
    const route = '/' + path.relative(root, file).replace(/index\.html$/, '');
    const source = fs.readFileSync(file, 'utf8');
    const redirect = source.match(/location\.replace\(['"]([^'"]+)/);
    if (redirect) aliases.push({route, destination: redirect[1]});
    else routes.add(route);
  }
  for (const file of files.filter(file => /\.(html|js)$/.test(file) && !file.includes(`${path.sep}lib${path.sep}`))) {
    const source = fs.readFileSync(file, 'utf8');
    for (const [, value] of source.matchAll(/["'`](\/(?:display\/|controller(?:[/?]|$)|[?])[^"'`<>\s]*|\/)["'`]/g)) {
      if (!value.includes('${') && !value.includes('\\')) links.add(value.replaceAll('&amp;', '&'));
    }
  }
  const canonical = new Map([...routes].map(route => [normalizeRoute(route), route]));
  const redirects = new Map(aliases.map(alias => [normalizeRoute(alias.route), alias.destination]));
  for (const destination of [...links, ...aliases.map(alias => alias.destination)]) {
    const url = new URL(destination, 'http://fixture');
    const pathname = normalizeRoute(url.pathname);
    if (canonical.has(pathname)) routes.add(canonical.get(pathname) + url.search);
    else if (!redirects.has(pathname)) throw new Error(`Navigation has no page: ${destination}`);
  }
  // These are select-driven states, so they have no literal navigation link.
  for (const route of ['/?radar_view=fusion', '/?radar_view=channel',
    '/display/configuration/?setup=1', '/display/statistics/?tab=adsb']) routes.add(route);
  return {pages, aliases, routes: [...routes], links: [...links]};
}

// Deliberately narrow: operational limits, diagnostics, provenance and safety
// wording are allowed. This is a copy regression guard, not a style classifier.
const filler = [
  /\blorem ipsum\b/i, /\bcoming soon\b/i, /\bunder construction\b/i,
  /\bplaceholder (?:text|copy|content)\b/i, /\bas an AI(?: language model)?\b/i,
  /\bthis (?:page|panel|section) (?:allows|enables) you to\b/i,
  /\b(?:seamlessly leverage|unlock (?:the )?full potential|delve into)\b/i,
  /\b(?:TODO|FIXME)\s*:/
];
function copyIssues(text) { return filler.filter(pattern => pattern.test(text)).map(String); }
module.exports = {routeInventory, normalizeRoute, copyIssues};
