'use strict';

(function () {
  const channelRoot = document.getElementById('channel-radar-view');
  const fusionRoot = document.getElementById('fusion-radar-view');
  const diagnostics = document.getElementById('fusion-diagnostics');
  const diagnosticsToggle = document.getElementById('fusion-diagnostics-toggle');
  const scopeStatus = document.getElementById('fusion-scope-status');
  if (!channelRoot || !fusionRoot || !diagnostics) return;
  const glanceBoard = document.createElement('section');
  glanceBoard.className = 'fusion-glance-board';
  glanceBoard.hidden = true;
  fusionRoot.querySelector('.fusion-plot-pane')?.append(glanceBoard);
  const emptySummary = document.createElement('section');
  emptySummary.className = 'fusion-empty-summary';
  emptySummary.hidden = true;
  fusionRoot.append(emptySummary);
  let context = null, display = null, fusionSelected = true;
  let selectedTargetId = null, lastDiagnosticResult = null, lastDiagnosticTracks = [];
  let lastCooperativeResult = null;
  const fusedHistory = radarDisplayLanes.network.history.fused;
  const diagnosticsKey = 'vectorwarp-fusion-details-open-v2';
  const finite = value => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value));
  const text = value => String(value ?? '').replace(/[&<>"']/g, character =>
    ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;', "'":'&#39;'}[character]));
  function setDiagnosticsOpen(open, persist = true) {
    const visible = Boolean(open);
    fusionRoot.classList.toggle('diagnostics-collapsed', !visible);
    diagnostics.hidden = !visible;
    if (diagnosticsToggle) {
      diagnosticsToggle.setAttribute('aria-expanded', String(visible));
      diagnosticsToggle.textContent = visible ? 'Hide details' : 'Show details';
    }
    if (persist) try { localStorage.setItem(diagnosticsKey, String(visible)); } catch (_) {}
    requestAnimationFrame(() => Plotly.Plots.resize('fusion-scope'));
  }
  const detailsParam = new URLSearchParams(location.search).get('fusion_details');
  let detailsOpen = detailsParam === 'closed' ? false : detailsParam === 'open' ? true : null;
  if (detailsOpen === null) try {
    const saved = localStorage.getItem(diagnosticsKey);
    detailsOpen = saved === null ? false : saved === 'true';
  } catch (_) { detailsOpen = false; }
  setDiagnosticsOpen(detailsOpen, false);
  diagnosticsToggle?.addEventListener('click', () =>
    setDiagnosticsOpen(fusionRoot.classList.contains('diagnostics-collapsed')));
  const local = (point, receiver) => model.radarPoint({
    latitude: point?.latitude ?? point?.lat, longitude: point?.longitude ?? point?.lon
  }, receiver);
  const independentSites = independentTransmitterSiteCount;
  function desired() {
    return fusionSelected && context?.enabled && independentSites(context.illuminators) > 1;
  }
  function switchView() {
    const fusion = desired();
    channelRoot.hidden = fusion; fusionRoot.hidden = !fusion;
    document.body.classList.toggle('fusion-scope-active', fusion);
    if (fusion) { window.stopVectorWarpChannelRadar?.(); display.start(); display.refresh(); }
    else { display.stop(); window.startVectorWarpChannelRadar?.(); Plotly.Plots.resize('data'); }
  }
  const model = window.vectorwarpFusionAircraft;
  let displayedAircraft = [], lastSelectedAnnouncement = '';
  const diagnosticsEnabled = new URLSearchParams(location.search).get('fusion_diagnostics') === '1';
  const legend = document.createElement('div');
  legend.className = 'fusion-aircraft-key';
  legend.innerHTML = '<span class="fusion-altitude-key"><b>Altitude</b><i></i><small>Ground</small><small>40k+ ft</small></span>' +
    '<span class="fusion-evidence-key detected"><i></i>Detected</span>' +
    '<span class="fusion-evidence-key tracked"><i></i>Radar tracked</span>' +
    '<span class="fusion-evidence-key fused"><i></i>Independent radar position</span>';
  scopeStatus.after(legend);
  const diagnosticControl = document.createElement('label');
  diagnosticControl.className = 'fusion-diagnostic-control';
  diagnosticControl.innerHTML = '<input type="checkbox" id="fusion-hypotheses"> Show diagnostic hypotheses';
  legend.append(diagnosticControl);
  const hypothesesToggle = diagnosticControl.querySelector('input');
  hypothesesToggle.checked = diagnosticsEnabled;
  hypothesesToggle.addEventListener('change', () => display.refresh());
  const announcement = document.createElement('p');
  announcement.className = 'fusion-sr-only';
  announcement.setAttribute('role', 'status');
  announcement.setAttribute('aria-live', 'polite');
  announcement.setAttribute('aria-atomic', 'true');
  fusionRoot.append(announcement);
  scopeStatus.setAttribute('aria-live', 'polite');
  scopeStatus.setAttribute('aria-atomic', 'true');
  diagnostics.removeAttribute('aria-live');
  diagnostics.setAttribute('aria-label', 'Aircraft inspector');
  glanceBoard.setAttribute('aria-label', 'Aircraft in scope');
  function setHtml(element, html) {
    if (element.innerHTML !== html) element.innerHTML = html;
  }
  function supportText(value) {
    const channels = value.channels || [];
    return channels.length ? `${channels.length > 1 ? window.VectorWarpVisualSemantics.labels.multiChannel : 'Channel support'} · ${channels.join(', ')}` : 'No channel support reported';
  }
  function recordDescription(value) {
    return `${model.sourceLabel(value.source)} · ${model.stateLabel(value.state)}${value.coasting ? ' · ADS-B coasting' : ''}`;
  }
  function selectAircraft(id) {
    selectedTargetId = id;
    renderAircraftList();
    diagnostic(lastDiagnosticResult || {}, lastDiagnosticTracks, lastCooperativeResult);
  }
  glanceBoard.addEventListener('click', event => {
    const button = event.target.closest('button[data-aircraft-id]');
    if (button) selectAircraft(button.dataset.aircraftId);
  });
  function renderAircraftList() {
    const nearest = [...displayedAircraft].sort((a,b) => Math.hypot(...a.point) - Math.hypot(...b.point));
    const distanceView = document.body.classList.contains('distance-view');
    const limit = 4;
    const selected = nearest.find(value => value.id === selectedTargetId);
    const shown = (distanceView ? nearest : [selected, ...nearest.filter(value => value !== selected)].filter(Boolean)).slice(0, limit);
    const focusedId = glanceBoard.contains(document.activeElement) ? document.activeElement.dataset.aircraftId : null;
    glanceBoard.hidden = false;
    setHtml(glanceBoard, `<header><strong>Nearest aircraft</strong><span>${shown.length} of ${displayedAircraft.length}</span></header><div>` +
      shown.map(value => { const support = supportText(value); const channels = value.channels?.length || 0;
        const evidence = value.source === 'radar' ? 'fused' : value.state;
        const label = value.source === 'radar' ? model.sourceLabel(value.source) : model.stateLabel(value.state);
        return `<button type="button" data-aircraft-id="${text(value.id)}" aria-label="${text(`${value.name}, ${Math.hypot(...value.point).toFixed(1)} kilometres, ${recordDescription(value)}, ${support}`)}" aria-pressed="${value.id === selectedTargetId}" class="fusion-aircraft-row"><strong>${text(value.name)}</strong><b>${Math.hypot(...value.point).toFixed(1)} km</b><span class="fusion-evidence-key ${evidence}">${evidence === 'adsb' ? '' : '<i aria-hidden="true"></i>'}${text(`${label}${value.coasting ? ' · coasting' : ''}`)}</span>${channels ? `<small title="${text(support)}">${channels} channel${channels === 1 ? '' : 's'}</small>` : ''}</button>`; }).join('') +
      (shown.length ? '' : '<p>No aircraft positions available.</p>') + '</div>');
    if (focusedId) [...glanceBoard.querySelectorAll('button')].find(button => button.dataset.aircraftId === focusedId)?.focus({preventScroll:true});
  }
  function diagnostic(result, tracks, cooperative = lastCooperativeResult) {
    lastDiagnosticResult = result; lastDiagnosticTracks = tracks; lastCooperativeResult = cooperative;
    const selected = displayedAircraft.find(item => item.id === selectedTargetId) || displayedAircraft[0];
    if (selected) selectedTargetId = selected.id;
    const channels = context?.illuminators || [];
    const labels = model.transmitterSiteLabels(channels);
    const fresh = result?.available === true;
    const diagnosticTracks = tracks.filter(item => item.diagnostic_only || item.operational_position === false || String(item.state).startsWith('SUPPORT_') || item.state === 'PERSISTENT_AMBIGUOUS');
    const title = selected ? selected.name : 'Aircraft inspector';
    const metrics = selected ? [
      ['Position source', model.sourceLabel(selected.source)],
      ['Radar evidence', model.stateLabel(selected.state)],
      ['Supporting channels', supportText(selected)],
      ['Receiver range', `${Math.hypot(...selected.point).toFixed(1)} km`],
      ...(selected.coasting ? [['Position freshness', `ADS-B coasting · ${selected.ageSeconds.toFixed(0)} s`]] : []),
      ...(selected.match ? [['Identity association', 'Held-out ADS-B proximity match']] : []),
      ...(selected.radar?.uncertainty ? [['Position uncertainty', `Horizontal σ ${finite(selected.radar.uncertainty.horizontal_sigma_km) ? Number(selected.radar.uncertainty.horizontal_sigma_km).toFixed(2) + ' km' : 'not reported'} · estimated`]] : [])
    ] : [];
    setHtml(diagnostics, `<h2>${text(title)}</h2><span class="fusion-status ${selected ? selected.source : 'neutral'}">${text(selected ? recordDescription(selected) : fresh ? 'Live · no aircraft positions' : 'Waiting for fresh radar data')}</span>` +
      metrics.map(([label,value]) => `<div class="fusion-metric"><span>${text(label)}</span><strong>${text(value)}</strong></div>`).join('') +
      `<div class="fusion-metric"><span>Configured sites / channels</span><strong>${independentSites(channels)} / ${channels.length}</strong></div>` +
      `<div class="fusion-metric"><span>Diagnostic hypotheses</span><strong>${diagnosticTracks.length} · ${hypothesesToggle.checked ? 'shown' : 'hidden'}</strong></div>` +
      (hypothesesToggle.checked ? diagnosticTracks.map(item => `<div class="fusion-channel"><strong>${text(item.id)}</strong><small>Diagnostic only · ${text(item.state)} · ${Number(item.supporting_cpis || item.supporting_frames || 0)} supporting CPIs</small></div>`).join('') : '') +
      `<details class="fusion-channel-details"><summary>Channels · ${channels.length}</summary><div>` +
      channels.map((item,index) => `<div class="fusion-channel"><strong>${text(item.id)}</strong><small>${finite(item.frequency_hz) ? (Number(item.frequency_hz)/1e6).toFixed(1) : '—'} MHz · ${text(labels[index])}</small></div>`).join('') + '</div></details>');
    const message = selected ? `${selected.name}. ${recordDescription(selected)}. ${supportText(selected)}.` : 'No aircraft positions available.';
    if (message !== lastSelectedAnnouncement) { announcement.textContent = message; lastSelectedAnnouncement = message; }
  }
  async function update() {
    if (fusionRoot.hidden) return;
    try {
      const bundle = await readRadarDisplayBundle('network');
      const {result, config, cooperative} = bundle;
      const receiver = config?.location?.rx;
      const unavailable=result?.available!==true;
      const fusedTracks = model.operationalTracks(result, receiver);
      const supportTracks = (result?.cross_frequency_support?.tracks || []).filter(item =>
        ['SUPPORT_ACTIVE','SUPPORT_COASTING'].includes(item.state) && Number(item.misses || 0) <= 2);
      const envelopeCandidates = (result.position_candidates || []).filter(item => item.operational_position === false);
      const tracks = [...fusedTracks,...supportTracks,...envelopeCandidates];
      updateNetworkDisplayHistory(bundle, {model, project: local, fusedTracks});
      const {adsbAircraft, trackPoints} = bundle;
      const nowMilliseconds = Date.now();
      const envelopeContours = hypothesesToggle.checked ? envelopeCandidates.map(item => ({item,
        points:(item.position_envelope?.vertices || []).map(vertex=>model.radarPoint(vertex,receiver)).filter(Boolean)})) : [];
      const distanceView = document.body.classList.contains('distance-view');
      displayedAircraft = model.aircraft(adsbAircraft, result, receiver, fusedHistory);
      if (!displayedAircraft.some(value => value.id === selectedTargetId)) selectedTargetId = displayedAircraft[0]?.id || null;
      renderAircraftList();
      const trailAircraft = displayedAircraft.filter(value => value.trail.length > 1);
      window.vectorwarpFusionTrailStatus = {updatedAt: nowMilliseconds,
        aircraft: displayedAircraft.length, trails: trailAircraft.length,
        trailPoints: trailAircraft.reduce((total,value) => total + value.trail.length, 0)};
      // Trails and labels have no markers. One canonical map glyph per aircraft.
      const traces = [{type:'scatter', mode:'markers', x:[0], y:[0], name:'Receiver',
        meta:{kind:'receiver'}, showlegend:false, hovertemplate:'Radar receiver<extra></extra>',
        marker:{size:9,color:'#f3eee9',symbol:'cross'}},
        ...trailAircraft.map(value => ({type:'scatter', mode:'lines',
          x:value.trail.map(sample=>sample.point[0]), y:value.trail.map(sample=>sample.point[1]),
          meta:{kind:'aircraft-trail',aircraftId:value.id}, showlegend:false, hoverinfo:'skip',
          line:{color:model.positionColor(value),width:distanceView?3:1.5}, opacity:.35})),
        ...displayedAircraft.map(value => ({type:'scatter', mode:'text',
          x:[value.point[0]], y:[value.point[1]], customdata:[value.id],
          meta:{kind:'aircraft',aircraftId:value.id,positionSource:value.source,evidenceState:value.state},
          name:value.name, text:[value.id===selectedTargetId || value.source==='radar' ? text(value.name) : ''], textposition:'top center',
          textfont:{size:distanceView?24:11,color:value.id===selectedTargetId?window.VectorWarpVisualSemantics.colors.selected:model.positionColor(value)},
          hovertext:[text(value.name)+'<br>'+text(recordDescription(value))+'<br>'+text(supportText(value))],
          hovertemplate:'%{hovertext}<extra></extra>', showlegend:false})),
        ...envelopeContours.map(({item,points})=>({type:'scatter',mode:'lines',
          x:points.map(p=>p[0]),y:points.map(p=>p[1]),meta:{kind:'diagnostic-envelope'},
          showlegend:false,line:{color:'#aaa',width:2,dash:'dot'},
          hovertemplate:text(`Diagnostic envelope · ${item.state}`)+'<extra></extra>'}))];
      // A stale/distant point must not crush the useful local operating area
      // into the centre of the plot. Frame the 90th-percentile coordinate
      // extent and cap the default scope; every aircraft remains available in
      // the list and by hover when it enters the visible area.
      const coordinateExtents = [...displayedAircraft.map(value=>value.point),
        ...envelopeContours.flatMap(value=>value.points), ...trackPoints.filter(Boolean)]
        .flatMap(point=>point.map(Math.abs)).filter(Number.isFinite).sort((a,b)=>a-b);
      const robustExtent = coordinateExtents.length ?
        coordinateExtents[Math.floor((coordinateExtents.length - 1) * .9)] : 60;
      const maximum = Math.min(70, Math.max(60, robustExtent * 1.1));
      const glyphSize = maximum * 2 * (distanceView ? 28 : 18) / Math.max(250, Math.min(fusionPlot.clientWidth, fusionPlot.clientHeight) - 100);
      for (const trace of traces) if (trace.meta?.kind === 'aircraft') trace.y[0] += glyphSize * .65;
      const images = displayedAircraft.map(value => ({name:value.id,
        source:'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(model.glyphMarkup(value,window.VectorWarpVisualSemantics.aircraftSvgMarkup(value.item,model.positionColor(value)))),
        xref:'x',yref:'y',x:value.point[0],y:value.point[1],sizex:glyphSize,sizey:glyphSize,
        xanchor:'center',yanchor:'middle',layer:'above',opacity:value.coasting?.55:1}));
      const shapes = [20,40,60,80].filter(r=>r<maximum*1.2).map(r=>({type:'circle',x0:-r,y0:-r,x1:r,y1:r,
        line:{color:'rgba(220,210,205,.22)',width:1}}));
      shapes.push({type:'line',x0:-maximum,y0:0,x1:maximum,y1:0,line:{color:'rgba(220,210,205,.25)'}},
        {type:'line',x0:0,y0:-maximum,x1:0,y1:maximum,line:{color:'rgba(220,210,205,.25)'}});
      // Match the four visible glyph categories: a solved radar position owns
      // its orange ring and is not counted again as an ADS-B-position track.
      const counts = {adsb:displayedAircraft.filter(value=>value.source==='adsb'&&value.state==='adsb').length,
        detected:displayedAircraft.filter(value=>value.source==='adsb'&&value.state==='detected').length,
        tracked:displayedAircraft.filter(value=>value.source==='adsb'&&value.state==='tracked').length,
        radar:displayedAircraft.filter(value=>value.source==='radar').length};
      const countLabels = [['adsb','ADS-B only'],['detected','Radar detected'],['tracked','Radar tracked'],['radar','Independent radar positions']];
      const summary = countLabels.map(([key,label])=>`${counts[key]} ${label}`).join(' · ');
      const radarStatus = unavailable ? 'Radar data unavailable' : 'Live';
      if (distanceView) setHtml(scopeStatus, countLabels.map(([key,label])=>
        `<span class="fusion-glance-metric ${key}"><strong>${counts[key]}</strong><small class="fusion-evidence-key ${key === 'radar' ? 'fused' : key}">${key === 'adsb' ? '' : '<i aria-hidden="true"></i>'}${label}</small></span>`).join('') +
        (unavailable ? '<span class="fusion-data-warning">Radar data unavailable</span>' : ''));
      else if(scopeStatus.textContent !== `${radarStatus} · ${summary}`) scopeStatus.textContent = `${radarStatus} · ${summary}`;
      scopeStatus.hidden = false;
      const emptyMode = !displayedAircraft.length;
      fusionRoot.classList.toggle('fusion-empty',emptyMode);
      emptySummary.hidden = true;
      fusionPlot.setAttribute('role','img');
      fusionPlot.setAttribute('aria-label',`Aircraft scope. ${radarStatus}. ${summary}. Select an aircraft in the list for position source and channel support.`);
      const axisFont={titlefont:{size:distanceView?19:12},tickfont:{size:distanceView?16:10}};
      const plotMargins={l:distanceView?20:58,r:distanceView?20:24,t:distanceView?16:48,b:distanceView?20:52};
      const innerWidth=Math.max(1,fusionPlot.clientWidth-plotMargins.l-plotMargins.r);
      const innerHeight=Math.max(1,fusionPlot.clientHeight-plotMargins.t-plotMargins.b);
      const xDomainWidth=Math.min(1,innerHeight/innerWidth);
      const xDomain=[(1-xDomainWidth)/2,(1+xDomainWidth)/2];
      const layout={title:{text:distanceView?'':'VectorWarp Fusion View',x:.03,font:{size:17}},
          xaxis:{title:distanceView?'':'East / west of receiver (km)',range:[-maximum,maximum],domain:xDomain,scaleanchor:'y',gridcolor:'rgba(0,0,0,0)',zeroline:false,showticklabels:!distanceView,...axisFont},
          yaxis:{title:distanceView?'':'North / south of receiver (km)',range:[-maximum,maximum],gridcolor:'rgba(0,0,0,0)',zeroline:false,showticklabels:!distanceView,...axisFont},shapes,images};
      const orientationAnnotations=[
        {x:0,y:maximum*.96,text:'N',xanchor:'center',yanchor:'top'},
        {x:maximum*.96,y:0,text:'E',xanchor:'right',yanchor:'middle'},
        {x:0,y:-maximum*.96,text:'S',xanchor:'center',yanchor:'bottom'},
        {x:-maximum*.96,y:0,text:'W',xanchor:'left',yanchor:'middle'},
        ...[20,40,60].filter(radius=>radius<=maximum).map(radius=>({x:radius*.71,y:radius*.71,text:`${radius} km`,xanchor:'left',yanchor:'bottom'}))
      ].map(item=>({xref:'x',yref:'y',showarrow:false,font:{size:distanceView?28:11,color:'rgba(243,238,233,.78)'},...item}));
      const annotations=[...orientationAnnotations,...(emptyMode?[{xref:'paper',yref:'paper',x:.5,y:.58,showarrow:false,align:'center',
        text:'<b>No aircraft positions available</b>',
        font:{size:distanceView?21:15,color:'#f3eee9'}}]:[])];
      await Plotly.react('fusion-scope',traces,{uirevision:'vectorwarp-fusion-view',transition:{duration:0},paper_bgcolor:'rgba(0,0,0,0)',plot_bgcolor:'rgba(0,0,0,0)',font:{color:'#f3eee9',size:distanceView?24:12},margin:plotMargins,...layout,annotations,showlegend:false},{responsive:true,displayModeBar:false});
      fusionPlot.querySelectorAll('.imagelayer image').forEach((image,index)=>{
        const value=displayedAircraft[index]; if(!value)return;
        image.style.pointerEvents='all';image.style.cursor='pointer';
        image.dataset.aircraftId=value.id;
        image.onclick=()=>selectAircraft(value.id);
      });
      if(fusionPlot?.on&&!fusionPlot.dataset.targetClickInstalled){
        fusionPlot.dataset.targetClickInstalled='true';
        fusionPlot.on('plotly_click',event=>{const id=event?.points?.[0]?.customdata;
          if(id)selectAircraft(id);});
      }
      diagnostic(result,tracks,cooperative);
    } catch (error) {
      fusedHistory.clear(); displayedAircraft=[]; renderAircraftList();
      scopeStatus.hidden=false;
      if(scopeStatus.textContent!=='Fusion data unavailable · retrying')scopeStatus.textContent='Fusion data unavailable · retrying';
      await Plotly.react('fusion-scope',[],{images:[],paper_bgcolor:'rgba(0,0,0,0)',plot_bgcolor:'rgba(0,0,0,0)',font:{color:'#f3eee9'},annotations:[]},{responsive:true,displayModeBar:false});
      setHtml(diagnostics,`<h2>Aircraft inspector</h2><span class="fusion-status neutral" title="${text(error.message)}">Waiting for fresh data</span>`);
    }
  }
  const fusionPlot=document.getElementById('fusion-scope');
  function installSelector(value) {
    context=value;
    const select=document.getElementById('shared-illuminator-select');
    if(select&&independentSites(context.illuminators)>1&&!select.querySelector('option[value="__fusion__"]')){
      const label=select.closest('label')?.querySelector('span'); if(label) label.textContent='View / channel';
      const option=document.createElement('option'); option.value='__fusion__';
      option.textContent='VectorWarp Fusion View'; select.prepend(option);
      const params=new URLSearchParams(location.search);
      if(params.get('radar_view')==='fusion')fusionSelected=true;
      else if(params.get('illuminator'))fusionSelected=false;
      else try{fusionSelected=localStorage.getItem('vectorwarp-live-radar-view')!=='channel';}catch(_){fusionSelected=true;}
      if(fusionSelected&&!params.has('radar_view')&&!params.has('illuminator')){
        const canonical=new URL(location.href);canonical.searchParams.set('radar_view','fusion');
        history.replaceState({vectorwarpView:'fusion'},'',canonical.href);
      }
      if(fusionSelected) select.value='__fusion__';
      select.addEventListener('change',()=>{
        fusionSelected=select.value==='__fusion__';
        const url=new URL(location.href);
        if(fusionSelected){url.searchParams.delete('illuminator');url.searchParams.set('radar_view','fusion');}
        else url.searchParams.delete('radar_view');
        history.replaceState(null,'',url.href);
        try{localStorage.setItem('vectorwarp-live-radar-view',fusionSelected?'fusion':'channel');}catch(_){}
        switchView();
      });
    } else if(!select||independentSites(context.illuminators)<=1) fusionSelected=false;
    switchView();
  }
  async function getFusionContext() {
    const localContext = await getIlluminatorContext();
    const fleet = await getFederatedChannels({fresh: true}).catch(() => null);
    if (!fleet?.channels?.length) return localContext;
    return {...localContext, enabled: true, nodes: fleet.nodes,
      illuminators: fleet.channels.map(channel => ({...channel.item,
        nodeId: channel.nodeId, nodeLabel: channel.nodeLabel}))};
  }
  startRadarDisplay('network', update, {paused: true}).then(controller => {
    display = controller;
    return getFusionContext().then(installSelector);
  });
  window.addEventListener('popstate',()=>getIlluminatorContext().then(value=>{
    context=value;const select=document.getElementById('shared-illuminator-select');
    const params=new URLSearchParams(location.search);fusionSelected=params.get('radar_view')==='fusion'||
      (!params.has('radar_view')&&!params.has('illuminator'));
    if(select)select.value=fusionSelected?'__fusion__':`${selectedNode()}::${context.id}`;switchView();
  }));
  window.addEventListener('vectorwarp:illuminator-change',event=>{context=event.detail;
    if(!document.querySelector('#shared-illuminator-select option[value="__fusion__"]'))installSelector(context);
    if(!fusionRoot.hidden) display.refresh();});
  window.addEventListener('vectorwarp:illuminator-catalog-ready',()=>getFusionContext().then(installSelector));
})();
