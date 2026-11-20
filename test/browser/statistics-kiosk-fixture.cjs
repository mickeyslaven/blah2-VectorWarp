'use strict';
// Synthetic four-channel evidence preserving realistic values and text widths,
// including TX-B 18.7% and TX-C 16.1% at 150% display scaling.
// It exercises the real frontend aggregation; there is no backend replacement.
const counts = [
  ['TX-A-RF27',68118,6765,4632], ['TX-B-RF25',51050,9546,7811],
  ['TX-C-RF28',63516,10226,8312], ['primary',91928,1691,1011]
];
module.exports = function statisticsKioskFixture(url, config, singleChannel = false) {
  const remote = url.pathname.startsWith('/api/federation/nodes/hp');
  const endpoint = url.pathname.replace('/api/federation/nodes/hp','');
  const selected = remote ? counts.slice(3) : counts.slice(0,3);
  if (endpoint === '/api/federation/nodes') return {nodes:[
    {id:'local',label:'HomePortal',local:true}, ...(singleChannel ? [] : [{id:'hp',label:'HP',local:false}])]};
  if (endpoint === '/api/system/status') return {serverId:'statistics-kiosk-fixture',
    radar:'receiving',processorFresh:true,adsb:'receiving',adsbEnabled:true};
  if (endpoint === '/api/illuminators/live' && singleChannel)
    return {enabled:false,primary:'primary',illuminators:[{id:'primary',available:true,frequency_hz:509e6}]};
  if (endpoint === '/api/illuminators/live') return {enabled:!remote,primary:selected[0][0],
    illuminators:selected.map(([id],i)=>({id,available:true,frequency_hz:(509+i*6)*1e6}))};
  if (endpoint === '/api/runtime/config' || endpoint === '/api/config') return config;
  if (singleChannel && ['/api/live-scorecard', '/api/live-scorecard/provisional'].includes(endpoint)) {
    const [,eligible,detected,tracked] = counts[1];
    const period = {eligible_passes:300,association_eligible_passes:239,
      detected_passes:156,tracked_passes:130,
      detection_yield:156/239,tracking_yield:130/239,
      cpi_exposure:{eligible_cpis:eligible+1000,association_eligible_cpis:eligible,
        detected_cpis:detected, operationally_tracked_cpis:tracked,
        eligible_seconds:(eligible+1000)*.5, association_eligible_seconds:eligible*.5,
        detection_fraction:detected/eligible,missed_detection_fraction:1-detected/eligible,
        operational_track_fraction:tracked/eligible,operationally_untracked_fraction:1-tracked/eligible}};
    return {updated_at:Date.now(),windows:{day:period,lifetime:period},
      data_quality:{exact_timed_cpi_frames:100,unique_cpi_frames:100},
      active_passes:[],recent_finalized_passes:[],limits:{}};
  }
  if (endpoint !== '/api/live-scorecard/association-basis') return undefined;
  return {channels:selected.map(([id,eligible,detected,tracked])=>({
    identity:{illuminator:id},updated_at:Date.now(),
    association_statistics:{eligible_adsb_passes:239,
      detection:{eligible_adsb_passes:239,associated_passes:156,yield:156/239},
      tracking:{detected_passes:156,associated_passes:130,yield:130/156}},
    passes:Array.from({length:239},(_,i)=>({hex:(0xabc000+i).toString(16),
      started_at:1700000000000,finalized_at:1700000060000,
      detected:i<156,tracked:i<130,cpi_exposure:i===0?{
        eligible_cpis:eligible+1000,association_eligible_cpis:eligible,
        detected_cpis:detected, operationally_tracked_cpis:tracked,
        eligible_seconds:(eligible+1000)*.5,association_eligible_seconds:eligible*.5}: {}}))
  }))};
};
