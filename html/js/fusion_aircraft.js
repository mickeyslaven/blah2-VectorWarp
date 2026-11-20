'use strict';

// Presentation only: positions come from ADS-B or an operational radar solver.
// Identity matches never supply coordinates to the radar solver.
(function (root) {
  const semantics = typeof module === 'object' && module.exports ? require('./common.js').radarVisualSemantics : root.VectorWarpVisualSemantics;
  const finite = value => value !== null && value !== undefined && value !== '' && Number.isFinite(Number(value));
  const hex = value => String(value || '').trim().toLowerCase();
  const unique = values => [...new Set(values.filter(value => typeof value === 'string' && value.trim()).map(value => value.trim()))];
  const colors = {adsb: semantics.colors.adsbFallback, detection: semantics.colors.detection,
    track: '#5be7ff', radar: semantics.colors.independent};
  function transmitterSiteKey(item = {}) {
    const transmitter = item.transmitter || item.transmitter_position || {};
    const latitude = transmitter.latitude ?? transmitter.lat;
    const longitude = transmitter.longitude ?? transmitter.lon;
    // Federation can omit transmitter_site on a relayed channel. Coordinates
    // remain authoritative and prevent a co-sited relay from becoming a fake
    // independent site in labels or geometry summaries.
    if (finite(latitude) && finite(longitude))
      return `geo:${Number(latitude).toFixed(5)}:${Number(longitude).toFixed(5)}`;
    const site = typeof item.transmitter_site === 'string' ? item.transmitter_site.trim() : '';
    return site || `channel:${String(item.id || '')}`;
  }
  function transmitterSiteLabels(items = []) {
    const coordinateSiteByFrequency = new Map();
    for (const item of items) {
      const key = transmitterSiteKey(item);
      const frequency = Number(item?.frequency_hz);
      if (key.startsWith('geo:') && finite(frequency))
        coordinateSiteByFrequency.set(frequency, key);
    }
    const sites = [];
    return items.map(item => {
      let key = transmitterSiteKey(item);
      // A federated single-channel node is exposed as `primary` and may lack
      // transmitter metadata. Reuse the authoritative catalog geometry only
      // when its exact tuned carrier is already present in this same view.
      if (key.startsWith('channel:') && String(item?.id || '').toLowerCase() === 'primary') {
        const catalogKey = coordinateSiteByFrequency.get(Number(item?.frequency_hz));
        if (catalogKey) key = catalogKey;
      }
      let index = sites.indexOf(key);
      if (index < 0) { sites.push(key); index = sites.length - 1; }
      return `Site ${index + 1}`;
    });
  }
  function state(association) {
    return association?.tracked === true ? 'tracked' : association?.detected === true ? 'detected' : 'adsb';
  }
  function stateLabel(value) {
    return {adsb: semantics.labels.adsbOnly, detected: semantics.labels.detected, tracked: semantics.labels.tracked}[value];
  }
  function sourceLabel(value) {
    return value === 'radar' ? semantics.labels.independent : semantics.labels.adsb;
  }
  function positionColor(value) {
    const item = value.item || {};
    const radarAltitudeFeet = finite(value.radar?.altitude) ? Number(value.radar.altitude) / .3048 : null;
    const altitude = item.alt_baro ?? item.alt_geom ?? item.altitude ??
      item.position?.altitude_baro_ft ?? radarAltitudeFeet;
    return finite(altitude) || altitude === 'ground' ?
      semantics.aircraftColor({...item, alt_geom: altitude}) : colors.adsb;
  }
  function radarPoint(item, receiver) {
    if (finite(item.local_east_km) && finite(item.local_north_km))
      return [Number(item.local_east_km), Number(item.local_north_km)];
    const position = item.position_hypothesis || item;
    if (![position.latitude, position.longitude, receiver?.latitude, receiver?.longitude].every(finite)) return null;
    return [(Number(position.longitude) - Number(receiver.longitude)) * 111.32 * Math.cos(Number(receiver.latitude) * Math.PI / 180),
      (Number(position.latitude) - Number(receiver.latitude)) * 111.32];
  }
  function operationalTracks(result, receiver) {
    if (result?.available !== true || result.adsb_used_by_solver === true) return [];
    const tracks = (result.tracks || []).filter(item => item.state === 'FUSED_ACTIVE');
    const fixes = (result.position_candidates || []).filter(item => item.operational_position === true &&
      item.representative_is_fix !== false && item.resolved !== false);
    return [...tracks, ...fixes].filter(item =>
      item.operational_position !== false && item.diagnostic_only !== true &&
      item.qualification?.qualified !== false && radarPoint(item, receiver));
  }
  function aircraft(adsb, result, receiver, histories = new Map()) {
    const records = new Map(adsb.map(value => [value.key, {...value, id: `adsb:${value.key}`,
      source: 'adsb', state: state(value.association),
      name: String(value.item.flight || value.item.hex || 'Aircraft').trim(),
      channels: unique(value.association?.channels || [])}]));
    const validation = result?.validation;
    const matches = validation?.held_out_truth === true && validation.adsb_used_by_solver !== true &&
      (!validation.target_kind || validation.target_kind === 'operational-fused-tracks') ? validation.matches || [] : [];
    const matchedHexes = new Set(), radarIds = new Set();
    for (const item of operationalTracks(result, receiver)) {
      if (!item.id || radarIds.has(item.id)) continue;
      radarIds.add(item.id);
      const match = matches.find(value => value.candidate_id === item.id && hex(value.adsb_hex));
      const identity = hex(match?.adsb_hex);
      // An explicitly matched aircraft gets one marker at the radar position.
      // Ambiguous duplicate identity claims are not resolved by client proximity.
      if (identity && matchedHexes.has(identity)) continue;
      if (identity) matchedHexes.add(identity);
      const original = records.get(identity);
      if (original) records.delete(identity);
      const channels = unique((item.contributors || []).map(value => value.illuminator || value.illuminator_id || value.channel));
      records.set(`radar:${item.id}`, {id: `radar:${item.id}`, key: identity || item.id, source: 'radar', state:item.state==='FUSED_ACTIVE'?'tracked':'detected',
        name: String(match?.adsb_flight || original?.item.flight || match?.adsb_hex || item.id).trim(), point: radarPoint(item, receiver),
        item: original?.item || {}, trail: histories.get(item.id) || [], channels, radar: item, match, coasting: false});
    }
    return [...records.values()];
  }
  function geographicPosition(item, receiver) {
    const position = item.position_hypothesis || item;
    if (finite(position.latitude) && finite(position.longitude)) return {lat:Number(position.latitude),lon:Number(position.longitude)};
    const point = radarPoint(item,receiver), scale = finite(receiver?.latitude) ? Math.cos(Number(receiver.latitude)*Math.PI/180) : 0;
    if (!point || !finite(receiver?.longitude) || Math.abs(scale)<.01) return null;
    return {lat:Number(receiver.latitude)+point[1]/111.32,lon:Number(receiver.longitude)+point[0]/(111.32*scale)};
  }
  function mapAircraft(rows, result, receiver) {
    const normalized = rows.map((item,index)=>({key:hex(item.hex || item.id || `row-${index}`),item,point:[],trail:[]}));
    return aircraft(normalized,result,receiver).map(value=>{
      if(value.source!=='radar')return value.item; // Existing ADS-B/Remote ID objects and styles remain intact.
      const position=geographicPosition(value.radar,receiver);
      if(!position)return null;
      const metadata=Object.fromEntries(['flight','r','t','aircraft_type','desc','category'].filter(key=>value.item[key]!==undefined)
        .map(key=>[key,value.item[key]]));
      const radar=value.radar, velocityKnown=finite(radar.velocity_east_km_s)&&finite(radar.velocity_north_km_s);
      return {...metadata,...position,hex:value.match?.adsb_hex || '',id:value.match?.adsb_hex || value.id,
        flight:value.name,position_source:'RADAR',seen_pos:finite(radar.measurement_timestamp)?Math.max(0,(Date.now()-Number(radar.measurement_timestamp))/1000):null,
        alt_geom:finite(radar.altitude)?Number(radar.altitude)/.3048:null,
        gs:velocityKnown?Math.hypot(radar.velocity_east_km_s,radar.velocity_north_km_s)*3600/1.852:null,
        track:velocityKnown?(Math.atan2(radar.velocity_east_km_s,radar.velocity_north_km_s)*180/Math.PI+360)%360:null,
        geom_rate:finite(radar.velocity_up_km_s)?Number(radar.velocity_up_km_s)*1000*60/.3048:null,
        radar_track_id:radar.state==='FUSED_ACTIVE'?radar.id:null,radar_track_state:radar.state,
        _radar_position:{id:radar.id,state:value.state,channels:value.channels,uncertainty:radar.uncertainty || null,
          identityMatched:Boolean(value.match)}};
    }).filter(Boolean);
  }
  function envelopeGeoJson(result, receiver, enabled=false) {
    const features=[];
    if(enabled && result?.available===true) for(const [index,item] of (result.position_candidates||[]).entries()) {
      if(item.operational_position!==false || item.state==='ENVELOPE_STALE')continue;
      const center=geographicPosition(item,receiver),radius=item.position_envelope?.horizontal_radius_km;
      if(!center || !finite(radius) || Number(radius)<=0)continue;
      const scale=Math.cos(center.lat*Math.PI/180);if(Math.abs(scale)<.01)continue;
      const ring=Array.from({length:65},(_,i)=>{
        const angle=i*2*Math.PI/64;
        return [center.lon+Number(radius)*Math.cos(angle)/(111.32*scale),center.lat+Number(radius)*Math.sin(angle)/111.32];
      });
      features.push({type:'Feature',id:item.id || `envelope-${index}`,properties:{state:item.state,
        diagnostic:true,operational_position:false,radius_km:Number(radius)},geometry:{type:'Polygon',coordinates:[ring]}});
    }
    return {type:'FeatureCollection',features};
  }
  function glyphMarkup(value, canonicalSvg) {
    const color = positionColor(value);
    const evidence = value.source === 'radar' ?
      `<circle class="fusion-evidence fused" cx="50" cy="50" r="42" fill="none" stroke="${colors.radar}" stroke-width="5"/>` :
      value.state === 'tracked' ?
        `<circle class="fusion-evidence tracked" cx="50" cy="50" r="42" fill="none" stroke="${colors.track}" stroke-width="4"/>` :
        value.state === 'detected' ?
          `<circle class="fusion-evidence detected" cx="82" cy="18" r="7" fill="${colors.detection}" stroke="#181311" stroke-width="3"/>` : '';
    const heading = value.source === 'radar' ?
      (finite(value.radar?.velocity_east_km_s) && finite(value.radar?.velocity_north_km_s) ?
        Math.atan2(value.radar.velocity_east_km_s, value.radar.velocity_north_km_s) * 180 / Math.PI : 0) :
      (finite(value.item?.track) ? Number(value.item.track) : 0);
    // Reuse the map's original planform, omitting its offset depth/highlight.
    // The map's own markup and artwork are untouched.
    return canonicalSvg.replace('<svg ', '<svg xmlns="http://www.w3.org/2000/svg" ')
      .replace(/<g class="aircraft-3d-depth"[^>]*>[\s\S]*?<\/g>/, '')
      .replace(/<path class="aircraft-3d-highlight"[^>]*\/>/, '')
      .replace(/class="aircraft-3d-top"/, `class="fusion-aircraft-planform" fill-opacity="1" transform="rotate(${heading} 50 50)"`)
      .replace('stroke-width="2"', 'stroke-width="2.5"')
      .replace('</svg>', `${evidence}</svg>`);
  }
  const api = {aircraft, operationalTracks, radarPoint, geographicPosition, mapAircraft, envelopeGeoJson,
    transmitterSiteKey, transmitterSiteLabels,
    positionColor, glyphMarkup, state, stateLabel, sourceLabel, colors};
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.vectorwarpFusionAircraft = api;
})(typeof window === 'object' ? window : globalThis);
