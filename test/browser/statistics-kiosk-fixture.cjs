'use strict';
// Synthetic four-channel evidence preserving pass/CPI diagnostics and exercising
// aircraft-time union coverage at portrait 150% display scaling.
// It exercises the real frontend aggregation; there is no backend replacement.
const counts = [
  ['TX-A-RF27',68118,6765,4632], ['TX-B-RF25',51050,9546,7811],
  ['TX-C-RF28',63516,10226,8312], ['primary',91928,1691,1011]
];
const {aggregate, kioskAssociationChannel} = require('../../html/js/measured_track_coverage');
const measuredBasis = index => ({version: 1, complete: true, aircraft: [{hex: 'abc123',
  eligible: [[0, 100000]], scored: process.env.VECTORWARP_COVERAGE_INCOMPLETE === '1' ? [] : [[0, 100000]], measured: [
    [[0, 10000], [20000, 50000]], [[0, 20000], [30000, 70000]],
    [[0, 20000], [30000, 80000]], [[0, 10000], [20000, 30000]]][index]}]});
module.exports = function statisticsKioskFixture(url, config, singleChannel = false) {
  const remote = url.pathname.startsWith('/api/federation/nodes/hp');
  const endpoint = url.pathname.replace('/api/federation/nodes/hp','');
  const selected = remote ? counts.slice(3) : counts.slice(0,3);
  if (endpoint === '/api/federation/nodes') return {nodes:[
    {id:'local',label:'HomePortal',local:true}, ...(singleChannel ? [] : [{id:'hp',label:'HP',local:false}])]};
  if (endpoint === '/api/system/status') return {serverId:'statistics-kiosk-fixture',
    radar:'receiving',processor:{state:'live'},processorFresh:!(remote && process.env.VECTORWARP_COVERAGE_MISSING_NODE === '1'),adsb:'receiving',adsbEnabled:true};
  if (endpoint === '/api/illuminators/live' && singleChannel)
    return {enabled:false,primary:'primary',illuminators:[{id:'primary',available:true,frequency_hz:509e6}]};
  if (endpoint === '/api/illuminators/live') return {enabled:!remote,primary:selected[0][0],
    illuminators:selected.map(([id],i)=>({id,available:true,frequency_hz:(509+i*6)*1e6}))};
  if (endpoint === '/api/runtime/config' || endpoint === '/api/config') return config;
  if (singleChannel && ['/api/live-scorecard', '/api/live-scorecard/provisional'].includes(endpoint)) {
    const [,eligible,detected,tracked] = counts[1];
    const period = {eligible_passes:300,association_eligible_passes:239,
      measured_track_coverage: aggregate([measuredBasis(1)]),
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
  const value = {channels:selected.map(([id,eligible,detected,tracked])=>({
    identity:{illuminator:id},updated_at:Date.now(),
    measured_track_basis: measuredBasis(counts.findIndex(row => row[0] === id)),
    association_statistics:{eligible_adsb_passes:239, evaluation_policy_hash: '5'.repeat(64),
      detection:{eligible_adsb_passes:239,associated_passes:156,yield:156/239},
      tracking:{detected_passes:156,associated_passes:130,yield:130/156}},
    passes:Array.from({length:239},(_,i)=>({hex:(0xabc000+i).toString(16),
      started_at:1700000000000,finalized_at:1700000060000,
      detected:i<156,tracked:i<130,cpi_exposure:i===0?{
        eligible_cpis:eligible+1000,association_eligible_cpis:eligible,
        detected_cpis:detected, operationally_tracked_cpis:tracked,
        eligible_seconds:(eligible+1000)*.5,association_eligible_seconds:eligible*.5}: {}}))
  }))};
  if (url.searchParams.get('display') === 'kiosk')
    value.channels = value.channels.map(kioskAssociationChannel);
  return value;
};
