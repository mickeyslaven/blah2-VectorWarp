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
  let context = null, timer = null, fusionSelected = true;
  let selectedTargetId = null, lastDiagnosticResult = null, lastDiagnosticTracks = [];
  let lastCooperativeResult = null;
  const fusedHistory = new Map();
  const adsbHistory = new Map();
  const adsbTrailHistory = new Map();
  const adsbCoastMilliseconds = 20000;
  const adsbTrailMilliseconds = 60000;
  const adsbTrailMaximumPoints = 60;
  const palette = ['#35cfe0','#69df7c','#e358c5','#ffb14a','#8f7cff'];
  const diagnosticsKey = 'vectorwarp-fusion-details-open';
  const finite = value => Number.isFinite(Number(value));
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
    detailsOpen = saved === null ? true : saved === 'true';
  } catch (_) { detailsOpen = true; }
  setDiagnosticsOpen(detailsOpen, false);
  diagnosticsToggle?.addEventListener('click', () =>
    setDiagnosticsOpen(fusionRoot.classList.contains('diagnostics-collapsed')));
  function local(point, receiver) {
    const latitude=point?.latitude??point?.lat,longitude=point?.longitude??point?.lon;
    if (!finite(latitude) || !finite(longitude) || !finite(receiver?.latitude) ||
        !finite(receiver?.longitude)) return null;
    const north = (Number(latitude)-Number(receiver.latitude))*111.32;
    const east = (Number(longitude)-Number(receiver.longitude))*111.32*
      Math.cos(Number(receiver.latitude)*Math.PI/180);
    return [east,north];
  }
  function independentSites(items) {
    if (typeof independentTransmitterSiteCount === 'function')
      return independentTransmitterSiteCount(items);
    const ids = new Set(items.map(item=>typeof item?.transmitter_site === 'string' ?
      item.transmitter_site.trim() : '').filter(Boolean));
    return ids.size;
  }
  function siteLabels(items) {
    const sites=[];return items.map(item=>{let index=sites.indexOf(item.transmitter_site);
      if(index<0){sites.push(item.transmitter_site);index=sites.length-1;}return `Site ${index+1}`;});
  }
  function desired() {
    return fusionSelected && context?.enabled && independentSites(context.illuminators) > 1;
  }
  function switchView() {
    const fusion = desired();
    channelRoot.hidden = fusion; fusionRoot.hidden = !fusion;
    document.body.classList.toggle('fusion-scope-active', fusion);
    if (fusion) { window.stopVectorWarpChannelRadar?.(); update(); timer ||= setInterval(update, 1000); }
    else { clearInterval(timer); timer = null; window.startVectorWarpChannelRadar?.(); Plotly.Plots.resize('data'); }
  }
  function diagnostic(result, tracks, cooperative = lastCooperativeResult) {
    lastDiagnosticResult=result;lastDiagnosticTracks=tracks;lastCooperativeResult=cooperative;
    const selected = tracks.find(item=>item.id===selectedTargetId)||tracks[0];
    if(selected)selectedTargetId=selected.id;
    const channels = context?.illuminators || [];
    const labels=siteLabels(channels);
    const match=selected&&!selected.diagnostic_only?(result.validation?.matches||[])
      .find(item=>item.candidate_id===selected.id):null;
    const supportSelected=String(selected?.state||'').startsWith('SUPPORT_');
    const title=supportSelected?`Cross-frequency support ${selected.id}`:selected?.diagnostic_only?`Ambiguous radar hypothesis ${selected.id}`:
      selected?`Confirmed radar track ${selected.id}`:'Fusion status';
    const fresh=result?.available===true||result?.map_support?.available===true;
    const badge=match?'ADS-B PROXIMITY MATCH':supportSelected?'DIAGNOSTIC SUPPORT':selected?.diagnostic_only?'DIAGNOSTIC ONLY':
      selected?'CONFIRMED RADAR':fresh?'LIVE · NO CONFIRMED TRACKS':'WAITING FOR FRESH DATA';
    const badgeClass=match?'adsb':selected?.diagnostic_only?'diagnostic':selected?'confirmed':'neutral';
    const cooperativeTracks = Array.isArray(cooperative?.tracks) ? cooperative.tracks : [];
    const contributingNodes = new Set(cooperativeTracks.flatMap(item =>
      item.recent_supporting_nodes || item.contributing_nodes || []));
    const configuredNodes = context?.nodes?.length || 1;
    diagnostics.innerHTML = `<h2>${text(title)}</h2>`+
      `<span class="fusion-status ${text(badgeClass)}">${text(badge)}</span>`+
      (match?`<div class="fusion-metric"><span>ADS-B aircraft</span><strong>${text(match.adsb_flight||match.adsb_hex||'Matched')}</strong></div>`+
        `<div class="fusion-metric"><span>Proximity</span><strong>${Number(match.error_km).toFixed(2)} km</strong></div>`+
        `<div class="fusion-metric"><span>Match time</span><strong>${finite(result.validation?.timestamp)?new Date(Number(result.validation.timestamp)).toLocaleTimeString():'Current cohort'}</strong></div>`:'')+
      `<div class="fusion-metric"><span>Independent sites</span><strong>${finite(result.map_support?.generation?.independent_transmitter_sites ?? result.independent_transmitter_sites) ? Number(result.map_support?.generation?.independent_transmitter_sites ?? result.independent_transmitter_sites) : independentSites(channels)}</strong></div>`+
      `<div class="fusion-metric"><span>Channels</span><strong>${channels.length}</strong></div>`+
      `<div class="fusion-metric"><span>Radar nodes</span><strong>${configuredNodes} connected · ${contributingNodes.size} contributing now</strong></div>`+
      `<div class="fusion-metric"><span>Cooperative hypotheses</span><strong>${cooperativeTracks.length}</strong></div>`+
      `<div class="fusion-metric"><span>Supporting CPIs</span><strong>${finite(selected?.supporting_cpis ?? selected?.supporting_frames) ? Number(selected?.supporting_cpis ?? selected?.supporting_frames) : 0}</strong></div>`+
      channels.map((item,index)=>`<div class="fusion-channel" style="border-left:4px solid ${palette[index%palette.length]}"><strong>${text(item.id)}${item.nodeLabel ? ` (${text(item.nodeLabel)})` : ''}</strong><small>${finite(item.frequency_hz) ? (Number(item.frequency_hz)/1e6).toFixed(1) : '—'} MHz · ${text(labels[index])} · configured</small></div>`).join('');
  }
  async function update() {
    if (fusionRoot.hidden) return;
    try {
      const [result, config, cooperative, adsbData, associationData] = await Promise.all([
        fetchRadarJson(liveApiUrl('/api/tracker/fused')),
        getRadarRuntimeConfig(),
        fetchRadarJson(liveApiUrl('/api/federation/cooperative')).catch(() => null),
        fetchRadarJson(liveApiUrl('/api/adsb')).catch(() => null),
        fetchRadarJson(liveApiUrl('/api/live-aircraft-associations')).catch(() => null)
      ]);
      const receiver = config?.location?.rx;
      const mapFresh=result?.map_support?.available===true,fusedFresh=result?.available===true;
      const unavailable=!mapFresh&&!fusedFresh;
      if (unavailable) fusedHistory.clear();
      const mapTracks = (mapFresh?result?.map_support?.tracks||[]:[]).filter(item=>
        item.state==='PERSISTENT_AMBIGUOUS' && item.misses===0);
      const fusedTracks = (fusedFresh?result?.tracks||[]:[]).filter(item=>item.state==='FUSED_ACTIVE');
      const matchById=new Map((result.validation?.held_out_truth===true?result.validation?.matches||[]:[])
        .filter(item=>item.candidate_id).map(item=>[item.candidate_id,item]));
      const supportTracks = (result?.cross_frequency_support?.tracks || []).filter(item =>
        ['SUPPORT_ACTIVE','SUPPORT_COASTING'].includes(item.state) && Number(item.misses || 0) <= 2);
      const tracks = [...fusedTracks,...mapTracks,...supportTracks];
      const associationByHex=new Map((associationData?.associations||[])
        .filter(item=>item?.hex).map(item=>[String(item.hex).trim().toLowerCase(),item]));
      const nowMilliseconds=Date.now(),currentAdsbKeys=new Set();
      (Array.isArray(adsbData?.aircraft)?adsbData.aircraft:[]).forEach((item,index)=>{
        const key=String(item.hex||item.flight||`anonymous-${index}`).trim().toLowerCase();
        const point=local(item,receiver);
        if(!point||Math.hypot(...point)>65)return;
        currentAdsbKeys.add(key);adsbHistory.set(key,{item,point,lastSeen:nowMilliseconds,
          association:associationByHex.get(key)||null});
      });
      const adsbAircraft=[];
      for(const [key,entry] of adsbHistory){
        const ageMilliseconds=nowMilliseconds-entry.lastSeen;
        if(ageMilliseconds>adsbCoastMilliseconds){adsbHistory.delete(key);continue;}
        const ageSeconds=Math.max(0,ageMilliseconds/1000),point=[...entry.point];
        if(ageSeconds>0&&finite(entry.item.gs)&&finite(entry.item.track)){
          const speedKilometresPerSecond=Number(entry.item.gs)*1.852/3600;
          const headingRadians=Number(entry.item.track)*Math.PI/180;
          point[0]+=speedKilometresPerSecond*ageSeconds*Math.sin(headingRadians);
          point[1]+=speedKilometresPerSecond*ageSeconds*Math.cos(headingRadians);
        }
        if(Math.hypot(...point)<=60){
          const coasting=!currentAdsbKeys.has(key);
          let trail=adsbTrailHistory.get(key)||[];
          const previous=trail.at(-1),stamp=nowMilliseconds;
          if(previous){
            const dt=(stamp-previous.stamp)/1000;
            const displacement=Math.hypot(point[0]-previous.point[0],point[1]-previous.point[1]);
            // A reused ADS-B identity or a bad position report must never draw
            // a line across the entire scope. This is deliberately looser than
            // the radar fuser's gate because ADS-B updates are less frequent.
            if(!(dt>0)||dt>25||displacement>.5+.65*dt)trail=[];
          }
          trail.push({point:[...point],stamp,coasting,association:entry.association});
          trail=trail.filter(sample=>stamp-sample.stamp<=adsbTrailMilliseconds)
            .slice(-adsbTrailMaximumPoints);
          adsbTrailHistory.set(key,trail);
          adsbAircraft.push({key,item:entry.item,point,trail,
            association:entry.association,coasting,ageSeconds});
        }
      }
      for(const key of adsbTrailHistory.keys())if(!adsbHistory.has(key))adsbTrailHistory.delete(key);
      const liveAdsbAircraft=adsbAircraft.filter(value=>!value.coasting);
      const coastingAdsbAircraft=adsbAircraft.filter(value=>value.coasting);
      const trackedAdsbAircraft=liveAdsbAircraft.filter(value=>value.association?.tracked===true);
      const detectedAdsbAircraft=liveAdsbAircraft.filter(value=>value.association?.detected===true&&
        value.association?.tracked!==true);
      const plainAdsbAircraft=liveAdsbAircraft.filter(value=>value.association?.detected!==true);
      const mapPoints = mapTracks.map(item=>local(item.position_hypothesis,receiver)).filter(Boolean);
      const trackPoints = fusedTracks.map(item => finite(item.local_east_km) ?
        [Number(item.local_east_km),Number(item.local_north_km)] : local(item.position_hypothesis || item,receiver));
      const activeIds=new Set(fusedTracks.map(item=>item.id));
      for(const id of fusedHistory.keys()) if(!activeIds.has(id)) fusedHistory.delete(id);
      fusedTracks.forEach((item,index)=>{const point=trackPoints[index];if(!point)return;
        const stamp=Number(item.measurement_timestamp??item.timestamp??result.timestamp);
        let history=fusedHistory.get(item.id)||[];const last=history.at(-1);
        if(last&&last.stamp===stamp) history[history.length-1]={point,stamp};
        else {
          const dt=last&&finite(stamp)&&finite(last.stamp)?(stamp-last.stamp)/1000:null;
          const distance=last?Math.hypot(point[0]-last.point[0],point[1]-last.point[1]):0;
          // Never draw across a retirement, long measurement gap, reused ID,
          // or displacement outside the fuser's physical aircraft envelope.
          if(last&&(!(dt>0)||dt>3||distance>.25+.55*dt)) history=[];
          history.push({point,stamp});
        }
        fusedHistory.set(item.id,history.slice(-20));});
      // Instantaneous nonlinear roots can switch branches from one CPI to the
      // next.  They are evidence, not tracks, and drawing them makes stationary
      // ambiguity look like a jumping aircraft.  Display only temporally held,
      // alpha-beta-smoothed ambiguous branches and operational fused tracks.
      const ambiguous=mapPoints;
      const distanceView = document.body.classList.contains('distance-view');
      if(distanceView){
        const compass=bearing=>['N','NE','E','SE','S','SW','W','NW'][Math.round(bearing/45)%8];
        const nearest=[...adsbAircraft].sort((left,right)=>Math.hypot(...left.point)-Math.hypot(...right.point)).slice(0,4);
        glanceBoard.hidden=false;
        glanceBoard.innerHTML=`<header><strong>NEAREST AIRCRAFT</strong><span>${adsbAircraft.length} IN SCOPE</span></header><div>`+
          nearest.map(value=>{const range=Math.hypot(...value.point),bearing=(Math.atan2(value.point[0],value.point[1])*180/Math.PI+360)%360;
            const tracked=value.association?.tracked===true,detected=value.association?.detected===true;
            const state=tracked?'tracked':detected?'detected':'adsb';
            const label=tracked?'RADAR TRACKED':detected?'RADAR DETECTED':value.coasting?'ADS-B COAST':'ADS-B';
            const name=text(String(value.item.flight||value.item.hex||'AIRCRAFT').trim());
            return `<article class="${state}"><i style="--bearing:${bearing.toFixed(0)}deg">▲</i><strong>${name}</strong><b>${range.toFixed(1)} km</b><small>${label} · ${compass(bearing)}</small></article>`;}).join('')+
          `</div>`;
      }else glanceBoard.hidden=true;
      const trailAircraft=adsbAircraft.filter(value=>value.trail.length>1);
      const adsbTrailTraces=trailAircraft.flatMap(value=>{
        const tracked=value.association?.tracked===true,detected=value.association?.detected===true;
        const coastFraction=value.coasting?Math.max(.18,1-value.ageSeconds/(adsbCoastMilliseconds/1000)):1;
        const color=tracked?`rgba(255,135,84,${(.78*coastFraction).toFixed(3)})`:
          detected?`rgba(255,135,84,${(.48*coastFraction).toFixed(3)})`:
            `rgba(77,212,176,${(.22*coastFraction).toFixed(3)})`;
        const trace={type:'scattergl',mode:'lines',x:value.trail.map(sample=>sample.point[0]),
          y:value.trail.map(sample=>sample.point[1]),name:`${String(value.item.flight||value.item.hex||'ADS-B').trim()} trail`,
          showlegend:false,hoverinfo:'skip',line:{color,width:distanceView?(tracked?9:detected?6:4):(tracked?5:detected?3:2),
            dash:'solid'}};
        if(!tracked&&!detected)return [trace];
        const glowColor=`rgba(255,135,84,${((tracked?.16:.10)*coastFraction).toFixed(3)})`;
        return [{...trace,name:`${trace.name} glow`,line:{color:glowColor,
          width:distanceView?(tracked?22:16):(tracked?13:9),dash:'solid'}},trace];
      });
      window.vectorwarpFusionTrailStatus={updatedAt:nowMilliseconds,
        aircraft:adsbAircraft.length,trails:trailAircraft.length,
        trailPoints:[...adsbTrailHistory.values()].reduce((total,trail)=>total+trail.length,0)};
      const geographicTraces = [{type:'scattergl',mode:'markers',x:[0],y:[0],name:'Receiver',
        hovertemplate:'Radar receiver<extra></extra>',marker:{size:13,color:'#f3eee9',line:{width:4,color:'#ff7a45'},symbol:'circle'}},
      ...adsbTrailTraces,
      {type:'scattergl',mode:'markers+text',x:plainAdsbAircraft.map(value=>value.point[0]),y:plainAdsbAircraft.map(value=>value.point[1]),
        customdata:plainAdsbAircraft.map(value=>value.item.hex||''),text:plainAdsbAircraft.map(value=>String(value.item.flight||value.item.hex||'').trim()),
        textposition:'top center',textfont:{size:distanceView?20:11,color:'rgba(77,212,176,.95)'},name:'ADS-B aircraft',
        hovertemplate:'%{text}<br>ADS-B %{customdata}<extra></extra>',marker:{size:distanceView?22:12,color:'#4dd4b0',line:{width:distanceView?4:2,color:'#b8ffe9'},symbol:'triangle-up'}},
      {type:'scattergl',mode:'markers+text',x:detectedAdsbAircraft.map(value=>value.point[0]),y:detectedAdsbAircraft.map(value=>value.point[1]),
        customdata:detectedAdsbAircraft.map(value=>[value.item.hex||'',(value.association?.channels||[]).join(', ')]),
        text:detectedAdsbAircraft.map(value=>String(value.item.flight||value.item.hex||'').trim()),textposition:'top center',
        textfont:{size:distanceView?21:12,color:'#ffb18f'},name:'Radar detected ADS-B',
        hovertemplate:'%{text}<br>Radar detection · %{customdata[1]}<br>ADS-B %{customdata[0]}<extra></extra>',
        marker:{size:distanceView?29:16,color:'#4dd4b0',line:{width:distanceView?7:4,color:'#ff8754'},symbol:'triangle-up'}},
      {type:'scattergl',mode:'markers+text',x:trackedAdsbAircraft.map(value=>value.point[0]),y:trackedAdsbAircraft.map(value=>value.point[1]),
        customdata:trackedAdsbAircraft.map(value=>[value.item.hex||'',(value.association?.channels||[]).join(', ')]),
        text:trackedAdsbAircraft.map(value=>String(value.item.flight||value.item.hex||'').trim()),textposition:'top center',
        textfont:{size:distanceView?23:13,color:'#ffb18f'},name:'Radar tracked ADS-B',
        hovertemplate:'%{text}<br>Radar track · %{customdata[1]}<br>ADS-B %{customdata[0]}<extra></extra>',
        marker:{size:distanceView?34:19,color:'#ff8754',line:{width:distanceView?6:3,color:'#b8ffe9'},symbol:'diamond'}},
      {type:'scattergl',mode:'markers+text',x:coastingAdsbAircraft.map(value=>value.point[0]),y:coastingAdsbAircraft.map(value=>value.point[1]),
        customdata:coastingAdsbAircraft.map(value=>[value.item.hex||'',value.ageSeconds.toFixed(0),
          value.association?.tracked===true?'radar tracked':value.association?.detected===true?'radar detected':'ADS-B only']),
        text:coastingAdsbAircraft.map(value=>String(value.item.flight||value.item.hex||'').trim()),
        textposition:'top center',textfont:{size:distanceView?18:11,color:'rgba(77,212,176,.62)'},name:'ADS-B coast',
        hovertemplate:'%{text}<br>%{customdata[2]} · coasting %{customdata[1]} s<br>ADS-B %{customdata[0]}<extra></extra>',
        marker:{size:coastingAdsbAircraft.map(value=>distanceView?(value.association?.tracked===true?28:21):(value.association?.tracked===true?16:12)),
          color:coastingAdsbAircraft.map(value=>value.association?.tracked===true?'rgba(255,135,84,.22)':'rgba(77,212,176,.14)'),
          line:{width:2,color:'rgba(184,255,233,.62)'},symbol:'triangle-up-open'}},
      {type:'scattergl',mode:'markers',x:ambiguous.map(p=>p[0]),y:ambiguous.map(p=>p[1]),
        customdata:mapTracks.map(item=>item.id),name:'Ambiguous hypothesis',
        hovertemplate:'Ambiguous hypothesis %{customdata}<br>Diagnostic only · not a track<extra></extra>',
        marker:{size:distanceView?19:11,color:'#ffcf5a',line:{width:distanceView?4:2,color:'#ffcf5a'},symbol:'circle-open'}},
      ...fusedTracks.map(item=>{const history=fusedHistory.get(item.id)||[],match=matchById.get(item.id);return {type:'scattergl',
        mode:history.length>1?'lines+markers+text':'markers+text',x:history.map(p=>p.point[0]),y:history.map(p=>p.point[1]),
        customdata:history.map(()=>item.id),name:match?`ADS-B match · ${match.adsb_flight||match.adsb_hex||item.id}`:`Fused track ${item.id}`,
        text:history.map((_,index)=>index===history.length-1?String(match?.adsb_flight||match?.adsb_hex||item.id):''),
        textposition:'top center',textfont:{size:distanceView?24:14,color:'#ffd2bf'},
        hovertext:history.map(()=>match?`${item.id} · ${item.state}<br>ADS-B proximity match ${match.adsb_flight||match.adsb_hex} · ${Number(match.error_km).toFixed(2)} km`:`${item.id} · ${item.state}`),
        hovertemplate:'%{hovertext}<extra></extra>',line:{color:'#ff7a45',width:distanceView?10:6},
        marker:{size:distanceView?(match?40:33):(match?22:18),color:'#ff7a45',line:{width:distanceView?(match?8:6):(match?5:3),color:match?'#ffffff':'#ffd2bf'},symbol:match?'star':'diamond'}};})];
      const traces = geographicTraces;
      const maximum = Math.max(60,...ambiguous.flatMap(p=>p.map(Math.abs)),...trackPoints.filter(Boolean).flatMap(p=>p.map(Math.abs)))*1.1;
      const shapes = [20,40,60,80].filter(r=>r<maximum*1.2).map(r=>({type:'circle',x0:-r,y0:-r,x1:r,y1:r,
        line:{color:'rgba(220,210,205,.22)',width:1}}));
      shapes.push({type:'line',x0:-maximum,y0:0,x1:maximum,y1:0,line:{color:'rgba(220,210,205,.25)'}},
        {type:'line',x0:0,y0:-maximum,x1:0,y1:maximum,line:{color:'rgba(220,210,205,.25)'}});
      const fusionSummary = `${context?.nodes?.length || 1} nodes · ${context?.illuminators?.length || 1} channels · ${Array.isArray(cooperative?.tracks) ? cooperative.tracks.length : 0} cooperative hypotheses`;
      if(distanceView&&!unavailable){
        const detectedCount=detectedAdsbAircraft.length+trackedAdsbAircraft.length;
        scopeStatus.innerHTML=`<span class="fusion-glance-metric adsb"><i></i><strong>${liveAdsbAircraft.length}</strong><small>ADS-B</small></span>`+
          `<span class="fusion-glance-metric detected"><i></i><strong>${detectedCount}</strong><small>RADAR<br>DETECTED</small></span>`+
          `<span class="fusion-glance-metric tracked"><i></i><strong>${trackedAdsbAircraft.length}</strong><small>RADAR<br>TRACKED</small></span>`+
          `<span class="fusion-glance-metric fused"><i></i><strong>${fusedTracks.length}</strong><small>MULTI-SITE<br>FUSED</small></span>`;
      }else scopeStatus.textContent=unavailable?(result?.map_support?.reason||result?.reason||'Waiting for fresh aligned radar evidence'):
        !fusedTracks.length?`Live · ${liveAdsbAircraft.length} ADS-B · ${detectedAdsbAircraft.length+trackedAdsbAircraft.length} radar detected · ${trackedAdsbAircraft.length} radar tracked${coastingAdsbAircraft.length?` · ${coastingAdsbAircraft.length} coasting`:''} · ${fusionSummary} · no confirmed fused tracks${supportTracks.length?` · ${supportTracks.length} diagnostic cross-frequency support`:''}`:
          `Live · ${liveAdsbAircraft.length} ADS-B · ${detectedAdsbAircraft.length+trackedAdsbAircraft.length} radar detected · ${trackedAdsbAircraft.length} radar tracked${coastingAdsbAircraft.length?` · ${coastingAdsbAircraft.length} coasting`:''} · ${fusionSummary} · ${fusedTracks.length} confirmed fused ${fusedTracks.length===1?'track':'tracks'}`;
      scopeStatus.hidden=!scopeStatus.textContent;
      const emptyMode=!unavailable&&!fusedTracks.length&&!ambiguous.length&&!adsbAircraft.length;
      fusionRoot.classList.remove('fusion-support-mode');
      fusionRoot.classList.toggle('fusion-empty',emptyMode);
      emptySummary.hidden=!emptyMode;
      if(emptyMode){const cooperativeTracks=Array.isArray(cooperative?.tracks)?cooperative.tracks:[];
        const activeCooperative=cooperativeTracks.filter(item=>item.state==='COOPERATIVE_ACTIVE').length;
        emptySummary.innerHTML=`<article><strong>${context?.nodes?.length||1}</strong><span>Radar nodes</span></article>`+
          `<article><strong>${context?.illuminators?.length||1}</strong><span>Channels</span></article>`+
          `<article><strong>${independentSites(context?.illuminators||[])}</strong><span>Independent transmitter sites</span></article>`+
          `<article><strong>${activeCooperative}</strong><span>Active cooperative tracks</span></article>`+
          `<p>Geographic fusion needs three independent transmitter sites. Cross-frequency support appears here automatically whenever it qualifies.</p>`;}
      const axisFont={titlefont:{size:distanceView?19:12},tickfont:{size:distanceView?16:10}};
      const layout={title:{text:distanceView?'':'VectorWarp Fusion View',x:.03,font:{size:17}},
          xaxis:{title:distanceView?'':'East / west of receiver (km)',range:[-maximum,maximum],scaleanchor:'y',gridcolor:'rgba(0,0,0,0)',zeroline:false,showticklabels:!distanceView,...axisFont},
          yaxis:{title:distanceView?'':'North / south of receiver (km)',range:[-maximum,maximum],gridcolor:'rgba(0,0,0,0)',zeroline:false,showticklabels:!distanceView,...axisFont},shapes};
      const orientationAnnotations=[
        {x:0,y:maximum*.96,text:'N',xanchor:'center',yanchor:'top'},
        {x:maximum*.96,y:0,text:'E',xanchor:'right',yanchor:'middle'},
        {x:0,y:-maximum*.96,text:'S',xanchor:'center',yanchor:'bottom'},
        {x:-maximum*.96,y:0,text:'W',xanchor:'left',yanchor:'middle'},
        ...[20,40,60].filter(radius=>radius<maximum).map(radius=>({x:radius*.71,y:radius*.71,text:`${radius} km`,xanchor:'left',yanchor:'bottom'}))
      ].map(item=>({xref:'x',yref:'y',showarrow:false,font:{size:distanceView?24:11,color:'rgba(243,238,233,.78)'},...item}));
      const annotations=[...orientationAnnotations,...(emptyMode?[{xref:'paper',yref:'paper',x:.5,y:.58,showarrow:false,align:'center',
        text:`<b>No confirmed fused tracks</b><br><span style="color:#ac9e94">${independentSites(context?.illuminators||[])} independent sites · geographic localization requires more qualified geometry</span>`,
        font:{size:distanceView?21:15,color:'#f3eee9'}}]:[])];
      await Plotly.react('fusion-scope',unavailable?[]:traces,{uirevision:'vectorwarp-fusion-view',paper_bgcolor:'rgba(0,0,0,0)',plot_bgcolor:'rgba(0,0,0,0)',font:{color:'#f3eee9',size:distanceView?20:12},margin:{l:distanceView?20:58,r:distanceView?20:24,t:distanceView?16:48,b:distanceView?20:52},...layout,annotations,showlegend:!distanceView&&!unavailable,legend:{x:.02,y:.98,font:{size:12}}},{responsive:true,displayModeBar:false});
      if(fusionPlot?.on&&!fusionPlot.dataset.targetClickInstalled){
        fusionPlot.dataset.targetClickInstalled='true';
        fusionPlot.on('plotly_click',event=>{const id=event?.points?.[0]?.customdata;
          if(id){selectedTargetId=id;diagnostic(lastDiagnosticResult||{},lastDiagnosticTracks,lastCooperativeResult);}});
      }
      diagnostic(result,tracks,cooperative);
    } catch (error) { fusedHistory.clear(); scopeStatus.hidden=false;scopeStatus.textContent='Fusion data unavailable · retrying'; await Plotly.react('fusion-scope',[],{paper_bgcolor:'rgba(0,0,0,0)',plot_bgcolor:'rgba(0,0,0,0)',font:{color:'#f3eee9'},annotations:[]},{responsive:true,displayModeBar:false}); diagnostics.innerHTML=`<h2>Fusion diagnostics</h2><span class="fusion-status neutral">WAITING FOR FRESH DATA</span><p>${text(error.message)}</p>`; }
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
    if (typeof getFederatedChannels !== 'function') return localContext;
    const fleet = await getFederatedChannels({fresh: true}).catch(() => null);
    if (!fleet?.channels?.length) return localContext;
    return {...localContext, enabled: true, nodes: fleet.nodes,
      illuminators: fleet.channels.map(channel => ({...channel.item,
        nodeId: channel.nodeId, nodeLabel: channel.nodeLabel}))};
  }
  getFusionContext().then(installSelector);
  window.addEventListener('popstate',()=>getIlluminatorContext().then(value=>{
    context=value;const select=document.getElementById('shared-illuminator-select');
    const params=new URLSearchParams(location.search);fusionSelected=params.get('radar_view')==='fusion'||
      (!params.has('radar_view')&&!params.has('illuminator'));
    if(select)select.value=fusionSelected?'__fusion__':`${selectedNode()}::${context.id}`;switchView();
  }));
  window.addEventListener('vectorwarp:illuminator-change',event=>{context=event.detail;
    if(!document.querySelector('#shared-illuminator-select option[value="__fusion__"]'))installSelector(context);
    if(!fusionRoot.hidden) update();});
  window.addEventListener('vectorwarp:illuminator-catalog-ready',()=>getFusionContext().then(installSelector));
})();
