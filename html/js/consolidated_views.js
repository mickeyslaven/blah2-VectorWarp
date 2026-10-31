(function () {
  'use strict';

  const kind = document.body.dataset.consolidatedView;
  const choices = {
    diagnostics: ['activity', 'timing', 'health'],
    signal: ['spectrum', 'maxhold'],
    detection: ['range', 'doppler', 'range-doppler']
  };
  const defaults = {diagnostics: 'activity', signal: 'spectrum', detection: 'range-doppler'};
  const requested = new URLSearchParams(window.location.search).get('view');
  const view = choices[kind]?.includes(requested) ? requested : defaults[kind];

  document.querySelectorAll('.view-tabs [data-view]').forEach(link => {
    const active = link.dataset.view === view;
    link.classList.toggle('active', active);
    if (active) link.setAttribute('aria-current', 'page');
    else link.removeAttribute('aria-current');
  });

  function load(source) {
    const script = document.createElement('script');
    script.src = source;
    script.defer = false;
    document.body.appendChild(script);
  }

  if (kind === 'diagnostics') {
    const content = document.getElementById('consolidated-content');
    const titles = {activity: 'Activity', timing: 'Timing', health: 'Health'};
    document.getElementById('view-title').textContent = titles[view];
    if (view === 'timing') {
      window.xTitle = 'Timestamp';
      window.yTitle = 'Time (ms)';
      content.innerHTML = '<section class="timing-summary" id="presentation-timing" aria-live="polite"><div><strong>Connecting</strong><span>Presentation timing</span></div></section><section class="panel"><div class="visualization" id="data"></div></section>';
      load('../../js/plot_timing.js?v=consolidated-pages-1');
    } else {
      document.body.dataset.insightView = view;
      content.innerHTML = '<div id="view-root"></div>';
      load('../../js/insights.js?v=frame-health-2');
    }
  } else if (kind === 'signal') {
    if (view === 'maxhold') {
      window.urlMap = '/stash/map';
      load('../../js/plot_map.js?v=consolidated-pages-1');
    } else load('../../js/plot_spectrum.js?v=consolidated-pages-1');
  } else if (kind === 'detection') {
    const variables = {
      range: ['Timestamp', 'Bistatic Range (km)', 'timestamp', 'delay'],
      doppler: ['Timestamp', 'Bistatic Doppler (Hz)', 'timestamp', 'doppler'],
      'range-doppler': ['Bistatic Range (km)', 'Bistatic Doppler (Hz)', 'delay', 'doppler']
    }[view];
    [window.xTitle, window.yTitle, window.xVariable, window.yVariable] = variables;
    load('../../js/plot_detection.js?v=consolidated-pages-1');
  }
}());
