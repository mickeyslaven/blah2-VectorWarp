'use strict';
const crypto = require('crypto');
const {buildDetectionCorpus} = require('./detection-manifold-corpus');
const finite = v => typeof v === 'number' && Number.isFinite(v);
const canonical = v => Array.isArray(v) ? `[${v.map(canonical).join(',')}]` : v && typeof v === 'object' ? `{${Object.keys(v).sort().map(k => `${JSON.stringify(k)}:${canonical(v[k])}`).join(',')}}` : JSON.stringify(v);
const hash = v => crypto.createHash('sha256').update(canonical(v)).digest('hex');
const add=(a,b)=>[a[0]+b[0],a[1]+b[1]], scale=(a,x)=>[a[0]*x,a[1]*x], multiply=(a,b)=>[a[0]*b[0]-a[1]*b[1],a[0]*b[1]+a[1]*b[0]], conjugate=a=>[a[0],-a[1]];
const inner=(a,b)=>a.reduce((s,v,i)=>add(s,multiply(conjugate(v),b[i])),[0,0]), power=v=>v[0]**2+v[1]**2;
const normalize=v=>{const n=Math.hypot(...v.flat());return n>1e-12&&finite(n)?v.map(x=>scale(x,1/n)):null;};
const angle=v=>((v%360)+360)%360, azimuthBin=(v,b)=>Math.floor(angle(v)/360*b)%b;
function aligned(v,r){const p=inner(r,v),n=Math.sqrt(power(p));return n>1e-10?v.map(x=>multiply(x,[p[0]/n,-p[1]/n])):null;}
function binFor(row,az,el,range){const width=Math.max(1e-9,range[1]-range[0]),elevation=Math.min(el-1,Math.max(0,Math.floor((row.truth.elevation_deg-range[0])/width*el))),azimuth=azimuthBin(row.truth.local_azimuth_deg,az);return {azimuth,elevation,key:`${azimuth}:${elevation}`};}
function idealVector(identity,azimuth,elevation){
  const az=azimuth*Math.PI/180,el=elevation*Math.PI/180;
  const direction=[Math.cos(el)*Math.cos(az),Math.cos(el)*Math.sin(az),Math.sin(el)];
  return normalize(identity.positions_m.map(position=>{
    const phase=-2*Math.PI*identity.frequency_hz/299792458*position.reduce((s,x,i)=>s+x*direction[i],0);
    return [Math.cos(phase),Math.sin(phase)];
  }));
}

// Shadow-only 2-D model. Sparse/unsupported cells deliberately have no response.
function fitDetectionManifold(rawSamples,{azimuthBins=8,elevationBins=4,minTrainingPassesPerBin=2,corpusSha256=null,elevationRangeDeg=[-90,90]}={}){
  if(!Number.isInteger(azimuthBins)||azimuthBins<4||azimuthBins>360||!Number.isInteger(elevationBins)||elevationBins<2||elevationBins>180||azimuthBins*elevationBins>4096||!Number.isInteger(minTrainingPassesPerBin)||minTrainingPassesPerBin<2||!Array.isArray(elevationRangeDeg)||elevationRangeDeg.length!==2||!elevationRangeDeg.every(finite)||elevationRangeDeg[0]<-90||elevationRangeDeg[1]>90||elevationRangeDeg[0]>=elevationRangeDeg[1])throw new Error('Detection shadow fitter requires a real two-dimensional azimuth/elevation grid');
  const corpus=buildDetectionCorpus(rawSamples,{corpusSha256});if(!corpus.training.length)throw new Error('No pass-disjoint detection training evidence');
  const first=corpus.training[0],range=[...elevationRangeDeg],groups=new Map();
  for(const row of corpus.training){if(row.truth.elevation_deg<range[0]||row.truth.elevation_deg>range[1])continue;const c=binFor(row,azimuthBins,elevationBins,range);if(!groups.has(c.key))groups.set(c.key,[]);groups.get(c.key).push(row);}
  const cells=[],responses=[];
  for(let azimuth=0;azimuth<azimuthBins;++azimuth)for(let elevation=0;elevation<elevationBins;++elevation){const key=`${azimuth}:${elevation}`,rows=groups.get(key)||[],passes=new Map();for(const row of rows){if(!passes.has(row._pass_id))passes.set(row._pass_id,[]);passes.get(row._pass_id).push(row);}const ideal=idealVector(first.manifold_identity,(azimuth+.5)*360/azimuthBins,range[0]+(elevation+.5)*(range[1]-range[0])/elevationBins);const independent_passes=passes.size,cell={key,azimuth_bin:azimuth,elevation_bin:elevation,training_samples:rows.length,ideal_response:ideal,elevation_range_deg:rows.length?[Math.min(...rows.map(r=>r.truth.elevation_deg)),Math.max(...rows.map(r=>r.truth.elevation_deg))]:null,independent_passes,confidence:Math.min(1,independent_passes/minTrainingPassesPerBin),support:independent_passes>=minTrainingPassesPerBin?'supported':independent_passes?'sparse':'unsupported',uncertainty:{method:'pass-balanced-projective-agreement-v1',azimuth_bin_width_deg:360/azimuthBins,elevation_bin_width_deg:(range[1]-range[0])/elevationBins,independent_passes}};if(cell.support==='supported'){let sum=ideal.map(()=>[0,0]);const coherence=[],reference=normalize(rows[0].response);let valid=!!reference;for(const passRows of passes.values()){let pass=ideal.map(()=>[0,0]);for(const row of passRows){const v=normalize(row.response);if(!v)throw new Error('Detection response has zero energy');const a=reference&&aligned(v,reference);if(!a){valid=false;break;}pass=pass.map((x,i)=>add(x,a[i]));}pass=normalize(pass);if(!pass){valid=false;continue;}coherence.push(pass);sum=sum.map((x,i)=>add(x,pass[i]));}const measured=valid&&normalize(sum),coherent=measured&&aligned(measured,ideal),weight=Math.min(.8,independent_passes/(independent_passes+4));if(coherent){cell.response=normalize(coherent.map((x,i)=>add(scale(x,weight),scale(ideal[i],1-weight))));cell.shrinkage_to_ideal=1-weight;cell.uncertainty.coherence=coherence.reduce((sum,pass)=>sum+Math.hypot(...inner(measured,pass)),0)/coherence.length;responses.push({...cell,response:cell.response});}else{cell.support='incoherent';cell.confidence=0;}}if(!cell.response){cell.fallback='ideal-for-diagnostic-comparison-only';cell.no_blind_extrapolation=true;}cells.push(cell);}
  const evidence_key=corpus.evidence_key,candidate_version=hash({schema:'vectorwarp-detection-manifold-candidate-v2',evidence_key,corpusSha256,azimuthBins,elevationBins,minTrainingPassesPerBin,range,ideal_method:'geometric-plane-wave-at-cell-centre-v1',cells:cells.map(c=>({...c,response:c.response||null}))});
  return {schema:'vectorwarp-detection-manifold-candidate-v2',candidate_version,runtime_compatible:false,intended_use:'offline-shadow-validation-only',frequency_hz:first.frequency_hz,sample_rate_hz:first.sample_rate_hz,channels:first.channels,channel_mapping:[...first.manifold_identity.channel_mapping],config_fingerprint:corpus.fingerprint,manifold_identity:first.manifold_identity,manifold_identity_sha256:hash(first.manifold_identity),evidence_key,azimuth_bins:azimuthBins,elevation_bins:elevationBins,elevation_range_deg:range,fixed_transmitter_anchor:{role:'wpxi-phase-frequency-drift-anchor',separate_from_aircraft_response_evidence:true,consumed_by_detection_fitter:false},ideal_manifold:{method:'geometric-plane-wave-at-cell-centre-v1',use:'shrinkage-only'},cells,responses,calibration:{method:'pass-balanced-2d-complex-mean-shrunk-to-ideal-v2',positive_corpus_sha256:corpusSha256,candidate_model_sha256:candidate_version,training_samples:corpus.training.length,training_passes:new Set(corpus.training.map(r=>r._pass_id)).size,independent_validation_samples:corpus.validation.length,independent_validation_passes:new Set(corpus.validation.map(r=>r._pass_id)).size,independent_miss_passes:corpus.miss_passes,adsb_role:corpus.labels}};
}
function responseFor(model,sample){
  const elevation=sample.truth?.elevation_deg;
  if(!finite(elevation)||!finite(sample.truth?.local_azimuth_deg)||elevation<model.elevation_range_deg[0]||elevation>model.elevation_range_deg[1])return null;
  const c=binFor(sample,model.azimuth_bins,model.elevation_bins,model.elevation_range_deg);
  const row=model.responses.find(r=>r.azimuth_bin===c.azimuth&&r.elevation_bin===c.elevation);
  return row&&elevation>=row.elevation_range_deg[0]&&elevation<=row.elevation_range_deg[1]?row.response:null;
}
function statistic(steering,vector,covariance){const numerator=power(inner(steering,vector)),applied=covariance.map(row=>row.reduce((s,v,i)=>add(s,multiply(v,steering[i])),[0,0])),denominator=inner(steering,applied)[0];return denominator>0&&finite(denominator)?numerator/denominator:null;}
function cfarThreshold(cfar){const p=-Math.expm1(Math.log1p(-cfar.pfa)/cfar.effective_trials);return cfar.training_cells*Math.expm1(-Math.log(p)/cfar.training_cells);}
const BANK_HASH_CONTRACT='vectorwarp-detection-bank-ieee754-v1';
function detectionBankHash(responses,directResponse){const digest=crypto.createHash('sha256'),beams=responses?.length,channels=directResponse?.length;if(!Number.isInteger(beams)||beams<1||!Number.isInteger(channels)||channels<1||responses.some(r=>!Array.isArray(r)||r.length!==channels)||[directResponse,...responses].some(r=>r.some(v=>!Array.isArray(v)||v.length!==2||!v.every(finite))))throw new Error('Invalid detection bank hash input');digest.update(Buffer.from(BANK_HASH_CONTRACT,'ascii'));const u=v=>{const b=Buffer.allocUnsafe(4);b.writeUInt32LE(v);digest.update(b);},f=v=>{const b=Buffer.allocUnsafe(8);b.writeDoubleLE(v===0?0:v);digest.update(b);};u(beams);u(channels);for(const r of [directResponse,...responses])for(const v of r){f(v[0]);f(v[1]);}return digest.digest('hex');}
function denseRuntimeCandidate(){throw new Error('Living detection manifold candidates are shadow-only and have no production conversion path');}
module.exports={BANK_HASH_CONTRACT,detectionBankHash,fitDetectionManifold,denseRuntimeCandidate,responseFor,statistic,cfarThreshold};
