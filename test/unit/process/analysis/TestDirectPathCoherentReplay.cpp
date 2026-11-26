#include "process/integration/DirectPathCoherentExperiment.h"
#include "ResidentReplayFixture.h"
#include "process/ambiguity/Ambiguity.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace fs = std::filesystem;
namespace rr = blah2::resident_replay;
namespace cr = blah2::integration::coherent_replay;

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& operation, const char* message) {
  bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; } check(rejected, message);
}
blah2::resident::Config config() {
  blah2::resident::Config value; value.inputChannels = 2; auto& g = value.geometry;
  g.range=12; g.doppler=7; g.delays=3; g.channels=2; g.delayMin=-1; g.clutterSamples=64; g.clutterBins=4;
  value.correlation=9; value.sampleRate=128000; value.epoch=9; value.surveillanceChannels={0,1}; value.delayAxis={-1,0,1}; const double r=1.0/(63.0/128000); value.dopplerAxis={-3*r,-2*r,-r,0,r,2*r,3*r}; value.spectrumBins=64;
  value.reference.channels={0,1}; value.reference.analysisSamples=64; value.detector.enabled=true; value.detector.guard=0; value.detector.train=2; value.detector.pfa=.001; value.detector.centroidDopplerResolution=2; value.detector.integrationCpis=1;
  value.device="cpu-contract-only"; rr::test::enable(value); return value;
}
cr::Sidecar sidecar(const rr::ResidentReplaySnapshot& snapshot, const blah2::resident::Config& value, uint64_t first, uint64_t epoch = 3, uint64_t tuning = 539000000, uint64_t gain = 2) {
  cr::Sidecar result; result.identity=snapshot.identity; result.sourceHash=value.replay.sourceHash; result.rfHash=value.replay.rfHash;
  result.captureEpoch=epoch; result.firstSample=first; result.endSample=first+64; result.tuningHz=tuning; result.gainCalibrationEpoch=gain; result.sampleRateHz=128000; result.actualGainDb=value.replay.actualGainDb; return result;
}
void write(const fs::path& path, const std::vector<uint8_t>& bytes) { std::ofstream output(path, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); check(bool(output), "snapshot write failed"); }
void populatePilot(rr::ResidentReplaySnapshot& snapshot) {
  auto& raw=snapshot.blocks.at(uint32_t(rr::BlockKind::rawIq)-1).bytes;
  auto& reference=snapshot.blocks.at(uint32_t(rr::BlockKind::referenceIq)-1).bytes;
  blah2::resident::wire::Writer rawWriter{{raw.data(),raw.size()}}, referenceWriter{{reference.data(),reference.size()}};
  for (size_t offset=0;offset<raw.size();offset+=16) rawWriter.f64(offset,1);
  for (size_t offset=0;offset<reference.size();offset+=16) referenceWriter.f64(offset,1);
  blah2::Sha256 hash; hash.update(raw.data(),raw.size()); snapshot.identity.rawIqHash=hash.finishBytes();
}
blah2::integration::DirectPathCoherentExperiment::Config experimentConfig() {
  blah2::integration::DirectPathCoherentExperiment::Config result; result.channelIds={0,1}; result.anchorChannelId=0; result.spatialCoefficients={1/std::sqrt(2.),{0,-1/std::sqrt(2.)}}; result.temporal.minimumCoherence=.1; return result;
}
void fractionalMaps(const fs::path& directory) {
  auto value=config(); value.detector.integrationCpis=8;
  value.detector.currentCpiPreferenceSnr=9.1;
  value.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,4,true);
  value.detector.integrationBeamTolerance=UINT32_MAX;
  value.fusion={blah2::resident::FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6,{}, {}};
  rr::test::enable(value); auto snapshot=rr::test::fixture(value);
  auto& maps=snapshot.blocks.at(uint32_t(rr::BlockKind::channelMaps)-1).bytes;
  blah2::resident::wire::Writer mapWriter{{maps.data(),maps.size()}};
  const auto rows=value.geometry.doppler,columns=value.geometry.delays;
  // Native buffers are channel-major, then delay-major, with unshifted Doppler.
  for(uint32_t channel=0;channel<value.geometry.channels;++channel)
    for(uint32_t column=0;column<columns;++column)for(uint32_t row=0;row<rows;++row){
      const size_t index=(size_t(channel)*columns+column)*rows+row;
      const float real=100*channel+10*column+row;
      mapWriter.f32(index*8,real);mapWriter.f32(index*8+4,-real);
    }
  const auto bytes=rr::encode(snapshot,value);const auto path=directory/"fractional.vwrps";
  write(path,bytes);cr::writeSidecarExclusive(path,sidecar(snapshot,value,100));
  const auto validated=cr::loadValidatedSnapshot(path);
  check(validated.config.detector.currentCpiPreferenceSnr==9.1,
    "Sidecar reader changed current-CPI merge threshold");
  check(validated.snapshot.blocks[4].bytes==snapshot.blocks[4].bytes,
    "Sidecar reader changed fractional metadata table");
  const auto decodedMaps=cr::channelMaps(validated);
  for(uint32_t channel=0;channel<value.geometry.channels;++channel){
    check(decodedMaps[channel].delay.size()==value.delayAxis.size()&&
      std::equal(value.delayAxis.begin(),value.delayAxis.end(),decodedMaps[channel].delay.begin())&&
      decodedMaps[channel].doppler.size()==value.dopplerAxis.size()&&
      std::equal(value.dopplerAxis.begin(),value.dopplerAxis.end(),decodedMaps[channel].doppler.begin()),
      "Fractional replay changed map axes");
    for(uint32_t row=0;row<rows;++row)for(uint32_t column=0;column<columns;++column){
      const double real=100*channel+10*column+(row+rows/2+1)%rows;
      check(decodedMaps[channel].data[row][column]==std::complex<double>(real,-real),
        "Fractional metadata tail changed native channel map order");
    }
  }
}
void hybridReplay(const fs::path& directory) {
  auto value = config(); value.detector.integrationCpis = 8;
  value.detector.integrationBeamTolerance = 0;
  value.fusion = {blah2::resident::FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6,{}, {}};
  rr::test::enable(value);
  cr::ExperimentReader reader(experimentConfig());
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    auto snapshot = rr::test::fixture(value, 1, {sequence, 100 + sequence, 1000 + sequence});
    populatePilot(snapshot);
    auto& power = snapshot.blocks.at(uint32_t(rr::BlockKind::currentPower) - 1).bytes;
    auto& fused = snapshot.blocks.at(uint32_t(rr::BlockKind::fused) - 1).bytes;
    blah2::resident::wire::Writer powerWriter{{power.data(), power.size()}},
      fusedWriter{{fused.data(), fused.size()}};
    for (size_t cell = 0; cell < power.size() / 8; ++cell) {
      powerWriter.f64(cell * 8, std::nextafter(1., 2.));
      fusedWriter.f64(cell * 16 + 8, sequence == 1 ? 0 : 1);
    }
    const auto path = directory / ("hybrid-" + std::to_string(sequence) + ".vwrps");
    write(path, rr::encode(snapshot, value));
    cr::writeSidecarExclusive(path, sidecar(snapshot, value, 100 + (sequence - 1) * 64));
    const auto rates = reader.appendAll(path);
    check(rates.size() == 1 && !rates[0].rateCorrectedFallback &&
      rates[0].declaredPerCellTrials == 1755 && rates[0].combinedPerCellTrials == 1755,
      "Legacy fallback beam trials were not charged");
    const auto& result = rates[0].result;
    const auto selected = result.hybridAt(0, 0);
    check(selected.valid == (sequence == 3), "Hybrid fallback ignored recorded beam compatibility");
    if (selected.valid) {
      check(!selected.coherent && selected.power == std::nextafter(1., 2.),
        "Hybrid fallback changed exact recorded power");
      check(selected.lineage.supportSequences() == std::vector<uint64_t>{3, 2} &&
        result.sources[0].firstSample == 228 && result.sources[1].endSample == 228,
        "Hybrid fallback lost exact source CPI lineage");
    }
  }
}
void composedMigration(const fs::path& directory) {
  using Experiment = blah2::integration::DirectPathCoherentExperiment;
  using Complex = Experiment::Complex;
  constexpr uint32_t samples = 1088, hop = 544, rateHz = 1088, rows = 17, columns = 49;
  constexpr double tau = 6.283185307179586476925286766559, doppler = 3.;
  constexpr uint64_t firstSample = 9007199254741000ULL;
  auto value = config();
  value.geometry = { }; auto& g = value.geometry;
  g.range = 90; g.doppler = rows; g.delays = columns; g.channels = 2;
  g.delayMin = -24; g.clutterSamples = samples; g.clutterBins = 4;
  value.correlation = 64; value.sampleRate = rateHz; value.delayAxis.clear(); value.dopplerAxis.clear();
  for (int col = -24; col <= 24; ++col) value.delayAxis.push_back(col);
  for (int row = -8; row <= 8; ++row) value.dopplerAxis.push_back(row);
  value.reference.analysisSamples = samples; value.detector.integrationCpis = 8;
  value.reference.bandwidthHz = 64; value.spectrumBins = 17;
  value.fusion = {blah2::resident::FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6,{}, {}};
  rr::test::enable(value);
  auto request = experimentConfig(); request.spatialCoefficients = {0., 1.};
  request.temporal.delayMotionBins = 1; request.temporal.dopplerMotionBins = 0;
  request.temporal.minimumCoherence = .8; request.migrationRates = {0., 2., -2.};
  cr::ExperimentReader reader(request);
  std::mt19937 random(445); std::uniform_real_distribution<double> uniform(-.5,.5);
  std::array<double, 32> frequencies{}, phases{};
  for (size_t i = 0; i < frequencies.size(); ++i) { frequencies[i] = .65 * uniform(random); phases[i] = tau * uniform(random); }
  const auto signal = [&](double position) {
    Complex z{}; for (size_t i = 0; i < frequencies.size(); ++i)
      z += std::polar(1., tau * frequencies[i] * position + phases[i]);
    return z / std::sqrt(double(frequencies.size()));
  };
  std::array<std::vector<Experiment::ComplexMap>, 3> history;
  std::vector<cr::ExperimentReader::RateResult> retained;
  for (uint64_t sequence = 1; sequence <= 9; ++sequence) {
    const double gauge = .37 * sequence;
    std::deque<Complex> reference; IqData target(samples);
    std::vector<Complex> physicalReference(samples), surveillance(samples);
    for (uint32_t n = 0; n < samples; ++n) {
      const double global = double((sequence - 1) * hop + n);
      const double delay = -5. + 2 * global / rateHz; // Two bins/s, one bin per overlapping hop.
      physicalReference[n] = signal(global);
      surveillance[n] = signal(global - delay) * std::polar(1., tau * doppler * global / rateHz);
      reference.push_back(physicalReference[n] * std::polar(1., gauge)); target.push_back(surveillance[n]);
    }
    Ambiguity ambiguity(-24, 24, -8, 8, rateHz, samples, true);
    check(ambiguity.get_n_doppler_bins() == rows && ambiguity.get_n_corr() == 64, "Composed IQ geometry differs");
    const auto caf = ambiguity.process(reference, &target);
    auto snapshot = rr::test::fixture(value, 1, {sequence, sequence, 1000 + sequence});
    const auto writer = [&](rr::BlockKind kind) { auto& bytes = snapshot.blocks.at(uint32_t(kind) - 1).bytes; return blah2::resident::wire::Writer{{bytes.data(),bytes.size()}}; };
    auto raw = writer(rr::BlockKind::rawIq), ref = writer(rr::BlockKind::referenceIq);
    auto maps = writer(rr::BlockKind::channelMaps), power = writer(rr::BlockKind::currentPower), fused = writer(rr::BlockKind::fused);
    for (uint32_t n = 0; n < samples; ++n) {
      const Complex anchor = sequence == 9 ? Complex{} : physicalReference[n];
      raw.f64(n * 16, anchor.real()); raw.f64(n * 16 + 8, anchor.imag());
      raw.f64((samples + n) * 16, surveillance[n].real()); raw.f64((samples + n) * 16 + 8, surveillance[n].imag());
      ref.f64(n * 16, reference[n].real()); ref.f64(n * 16 + 8, reference[n].imag());
    }
    for (uint32_t row = 0; row < rows; ++row) for (uint32_t col = 0; col < columns; ++col) {
      const size_t native = (size_t(columns) + col) * rows + (row + rows / 2 + 1) % rows;
      maps.f32(native * 8, float(caf->data[row][col].real())); maps.f32(native * 8 + 4, float(caf->data[row][col].imag()));
      power.f64((size_t(row) * columns + col) * 8, 1e12); // Must never enter corrected fallback.
      fused.f64((size_t(row) * columns + col) * 16 + 8, col % value.fusion.beams);
    }
    const auto& rawBytes = snapshot.blocks.at(uint32_t(rr::BlockKind::rawIq) - 1).bytes;
    blah2::Sha256 hash; hash.update(rawBytes.data(), rawBytes.size()); snapshot.identity.rawIqHash = hash.finishBytes();
    const auto path = directory / ("composed-" + std::to_string(sequence) + ".vwrps");
    write(path, rr::encode(snapshot, value));
    auto source = sidecar(snapshot, value, firstSample + (sequence - 1) * hop);
    source.endSample = source.firstSample + samples; source.sampleRateHz = rateHz;
    cr::writeSidecarExclusive(path, source);
    const auto sealed = cr::loadValidatedSnapshot(path);
    if (sequence == 1) {
      rejects([&] { reader.append(sealed); }, "Legacy append silently selected a migration rate");
      auto bad = sealed; --bad.sidecar.endSample;
      rejects([&] { reader.appendAll(bad); }, "Wrong raw span seeded rate histories");
    }
    if (sequence == 2) {
      auto bad = sealed; bad.sidecar.identity.configHash[0] ^= 1;
      rejects([&] { reader.appendAll(bad); }, "Changed config seeded rate histories");
      bad = sealed; bad.sidecar.firstSample += 3 * samples; bad.sidecar.endSample += 3 * samples;
      rejects([&] { reader.appendAll(bad); }, "A raw-IQ gap bridged migration histories");
    }
    const auto decoded = cr::channelMaps(sealed);
    const auto migrated = blah2::integration::RangeMigrationReplay::process(decoded[1], rateHz, 64, request.migrationRates);
    for (size_t trial = 0; trial < history.size(); ++trial) {
      Experiment::ComplexMap expected(rows, migrated.columns);
      expected.delay.assign(value.delayAxis.begin() + migrated.firstColumn, value.delayAxis.begin() + migrated.firstColumn + migrated.columns);
      for (uint32_t row = 0; row < rows; ++row) for (uint32_t col = 0; col < migrated.columns; ++col)
        expected.data[row][col] = migrated.banks[trial].cells[size_t(row) * migrated.columns + col] *
          std::polar(1., gauge);
      history[trial].push_back(std::move(expected));
    }
    auto result = reader.appendAll(sealed);
    check(result.size() == 3, "Composed reader spliced a winning migration rate");
    for (size_t trial = 0; trial < result.size(); ++trial) {
      const auto& rate = result[trial]; const auto& experiment = rate.result;
      check(rate.rateIndex == trial && rate.delayRateBinsPerSecond == request.migrationRates[trial] &&
        rate.rateCorrectedFallback && rate.firstColumn == migrated.firstColumn &&
        rate.source.firstSample == source.firstSample && rate.source.endSample == source.endSample &&
        rate.source.identity.configHash == sealed.sidecar.identity.configHash && rate.correlationSamples == 64,
        "Composed migration lost rate/raw/config lineage");
      check(rate.declaredPerCellTrials == 18 && rate.combinedPerCellTrials == 54,
        "Composed migration did not charge depth/path/rate/fallback trials");
      check(bool(rate.measuredPilot)==(sequence!=9) && (sequence==9 ||
        std::abs(rate.measuredPilot->normalizedCorrelation-std::polar(1.,-gauge))<1e-12),
        "Composed result lost measured pilot phase or its failure state");
      check(rate.recordedBeams.size() == rows * migrated.columns &&
        rate.recordedBeams[0] == migrated.firstColumn % value.fusion.beams + 1,
        "Recorded beam lineage did not use the same delay crop");
      const uint32_t col = uint32_t(int(sequence) - 5 - rate.fallback->delay.front());
      const size_t cell = 11 * migrated.columns + col;
      check(std::abs(rate.fallback->data[11][col].real() - std::norm(history[trial].back().data[11][col])) < 1e-6,
        "Corrected fallback used native power or a different rate");
      for (size_t bank = 0; bank < 3; ++bank) {
        const uint32_t depth = 2u << bank;
        if (sequence < depth || sequence == 9) continue;
        const auto& coherent = experiment.banks[bank]; const auto selected = experiment.hybridAt(bank, cell);
        check(coherent.ready && selected.valid && selected.coherent && coherent.motion[cell].delay == 1,
          "Continuous-IQ coherent path did not retain its one-bin motion");
        Complex oracle{};
        for (uint32_t age = 0; age < depth; ++age)
          oracle += history[trial][sequence - 1 - age].data[11][col - age] * std::polar(1., tau * doppler * age * hop / rateHz);
        oracle /= std::sqrt(double(depth));
        check(std::abs(selected.amplitude - oracle) < 1e-7 * std::max(1., std::abs(oracle)),
          "Composed 2/4/8-CPI complex phase differs from continuous-IQ oracle");
        check(selected.lineage.firstSamples[0] == source.firstSample &&
          selected.lineage.firstSamples[depth - 1] == source.firstSample - (depth - 1) * hop,
          "Composed coherent bank lost exact large sample counters");
      }
      if (sequence == 9) {
        const auto selected = experiment.hybridAt(2, cell);
        check(selected.valid && !selected.coherent && selected.power < 1e10 &&
          selected.lineage.depth == 8 && selected.lineage.firstSamples[0] == source.firstSample,
          "Pilot failure lost rate-corrected eight-CPI fallback or lineage");
      }
    }
    if (sequence == 1) retained = std::move(result);
    else check(retained[0].result.powerFallback == retained[0].fallback.get() &&
      std::isfinite(retained[0].fallback->data[0][0].real()), "Later append invalidated owned rate fallback");
    rejects([&] { reader.appendAll(sealed); }, "Duplicate source frame bridged rate histories");
  }
}
void overlappingFractionalReader(const fs::path& directory) {
  using Experiment = blah2::integration::DirectPathCoherentExperiment;
  constexpr uint32_t samples=1088, hop=544, fsHz=1088, rows=17, columns=49;
  constexpr double carrier=16320, tau=6.283185307179586476925286766559;
  auto value=config(); auto& g=value.geometry;
  g.range=90;g.doppler=rows;g.delays=columns;g.channels=2;g.delayMin=-24;
  g.clutterSamples=samples;value.correlation=64;value.sampleRate=fsHz;
  value.delayAxis.clear();value.dopplerAxis.clear();
  for(int c=-24;c<=24;++c)value.delayAxis.push_back(c);
  for(int r=-8;r<=8;++r)value.dopplerAxis.push_back(r);
  value.reference.analysisSamples=samples;value.reference.bandwidthHz=64;value.spectrumBins=17;
  value.detector.integrationCpis=8;
  value.detector.integrationMotionBins=track_before_detect::encodeMotionBins(0,0,true);
  value.fusion={blah2::resident::FusionMode::coherentRegularPolygon,12,carrier,.2,0,-1.2566370614359172,-.75,6,{}, {}};
  rr::test::enable(value);value.replay.frequencyHz=carrier;
  auto request=experimentConfig();request.spatialCoefficients={1.,0.};request.migrationRates={0.};
  request.temporal.delayMotionBins=0;request.temporal.dopplerMotionBins=0;
  request.temporal.minimumCoherence=.99;
  cr::ExperimentReader reader(request);
  auto directConfig=request;directConfig.migrationRates.clear();
  directConfig.temporal.interpolateDelay=true;
  directConfig.temporal.delayDriftPerHz=track_before_detect::delayDriftPerHz(hop,carrier);
  Experiment direct(directConfig,true);
  for(uint64_t sequence=1;sequence<=8;++sequence) {
    auto snapshot=rr::test::fixture(value,1,{sequence,sequence,1000+sequence});populatePilot(snapshot);
    auto& bytes=snapshot.blocks.at(uint32_t(rr::BlockKind::channelMaps)-1).bytes;
    blah2::resident::wire::Writer writer{{bytes.data(),bytes.size()}};
    for(uint32_t r=0;r<rows;++r)for(uint32_t c=0;c<columns;++c) {
      const double hz=value.dopplerAxis[r], walk=hz*hop/carrier;
      const auto z=std::polar(1.,tau*(.125*(value.delayAxis[c]+walk*(sequence-1))+hz*hop*(sequence-1)/fsHz));
      const auto native=size_t(c)*rows+(r+rows/2+1)%rows;
      writer.f32(native*8,float(z.real()));writer.f32(native*8+4,float(z.imag()));
    }
    const auto path=directory/("fractional-overlap-"+std::to_string(sequence)+".vwrps");
    write(path,rr::encode(snapshot,value));
    auto source=sidecar(snapshot,value,9007199254741000ULL+(sequence-1)*hop);
    source.endSample=source.firstSample+samples;source.sampleRateHz=fsHz;source.tuningHz=uint64_t(carrier);
    cr::writeSidecarExclusive(path,source);const auto sealed=cr::loadValidatedSnapshot(path);
    const auto decoded=cr::channelMaps(sealed);
    const auto migrated=blah2::integration::RangeMigrationReplay::process(decoded[0],fsHz,64,{0.});
    Experiment::ComplexMap map(rows,migrated.columns),zero(rows,migrated.columns),power(rows,migrated.columns);
    map.delay.assign(value.delayAxis.begin()+migrated.firstColumn,value.delayAxis.begin()+migrated.firstColumn+migrated.columns);
    map.doppler.assign(value.dopplerAxis.begin(),value.dopplerAxis.end());
    zero.delay=power.delay=map.delay;zero.doppler=power.doppler=map.doppler;
    for(uint32_t r=0;r<rows;++r)for(uint32_t c=0;c<migrated.columns;++c) {
      map.data[r][c]=migrated.banks[0].cells[size_t(r)*migrated.columns+c];
      power.data[r][c]={std::norm(map.data[r][c]),0};
    }
    Experiment::Frame frame{source.captureEpoch,sequence,source.firstSample,source.endSample,
      source.tuningHz,source.gainCalibrationEpoch,fsHz,true,true};
    Experiment::Pilot pilot{frame,0,1.,1.};
    auto expected=direct.process({&map,&zero},{0,1},frame,pilot,power);
    const auto actual=reader.appendAll(sealed);const auto& rate=actual[0];
    check(rate.hopSamples==(sequence==1?0:hop)&&
      rate.delayDriftPerHz==(sequence==1?0:directConfig.temporal.delayDriftPerHz),
      "Fractional replay bound CPI span instead of raw hop");
    const auto cell=size_t(11)*migrated.columns+24-migrated.firstColumn;
    for(size_t b=0;b<3;++b)if(sequence>=(2u<<b)) {
      const auto a=rate.result.hybridAt(b,cell),e=expected.hybridAt(b,cell);
      check(a.coherent&&e.coherent&&std::abs(a.amplitude-e.amplitude)<1e-12,
        "Overlapping fractional CPU/replay complex paths differ");
      check(rate.result.banks[b].coherence[cell]>.9999&&
        std::abs(a.amplitude-std::sqrt(double(2u<<b))*map.data[11][cell%migrated.columns])<.005,
        "Fractional overlap did not follow .1 bin per hop rather than .2 per span");
      check(rate.result.noncoherent.banks[b+1].detectorPower[cell]==expected.noncoherent.banks[b+1].detectorPower[cell],
        "Overlap binding changed canonical nearest-cell power fallback");
    }
    if(sequence==8) {
      auto bad=sealed;bad.sidecar.identity.logicalFrame++;bad.sidecar.identity.generation++;
      bad.sidecar.identity.consumedSequence++;bad.sidecar.firstSample+=hop+1;bad.sidecar.endSample+=hop+1;
      rejects([&]{reader.appendAll(bad);},"Changed overlap hop entered immutable rate history");
    }
  }
}
void fractionalPhase() {
  using Experiment = blah2::integration::DirectPathCoherentExperiment;
  using Complex = Experiment::Complex;
  Experiment::Config config; config.channelIds={0};config.anchorChannelId=0;config.spatialCoefficients={1.};
  config.temporal.delayMotionBins=0;config.temporal.dopplerMotionBins=0;
  config.temporal.minimumCoherence=.99;config.temporal.refineDopplerPhase=true;
  Experiment refined(config,true);auto baselineConfig=config;baselineConfig.temporal.refineDopplerPhase=false;
  Experiment baseline(baselineConfig,true);Experiment::ComplexMap map(3,1),power(3,1);
  map.delay={0};map.doppler={-10.,0.,10.};power.delay=map.delay;power.doppler=map.doppler;
  Experiment::Result result,unrefined;
  constexpr uint64_t first=9007199254741000ULL;
  for(uint64_t sequence=1;sequence<=8;++sequence){
    const Experiment::Frame frame{3,sequence,first+(sequence-1)*8,first+sequence*8,509000000,1,80,true,true};
    map.data[1][0]=std::polar(1.,2*std::acos(-1.)*2.5*.1*double(sequence-1));power.data[1][0]=1.;
    const Experiment::Pilot pilot{frame,0,1.,1.};
    result=refined.process({&map},{0},frame,pilot,power);
    unrefined=baseline.process({&map},{0},frame,pilot,power);
  }
  check(result.declaredPerCellTrials==28,"Fractional phase hypotheses were not charged");
  for(size_t bank=0;bank<3;++bank){
    const auto& value=result.banks[bank];
    check(value.valid[1]&&std::abs(std::norm(value.map->data[1][0])-double(2u<<bank))<1e-11&&
      std::abs(value.dopplerOffsetHz[1]-2.5)<1e-12,"Off-grid 2/4/8 phase refinement lost coherent gain or frequency");
    check(!unrefined.banks[bank].valid[1]&&unrefined.hybridAt(bank,1).power==1.,
      "Off-grid baseline did not preserve noncoherent fallback");
    check(value.lineageAt(1).firstSamples[0]==first+56,"Fractional phase lost exact large sample counter");
  }
  config.temporal.minimumCoherence=0;baselineConfig.temporal.minimumCoherence=0;
  Experiment noiseRefined(config,true),noiseBaseline(baselineConfig,true);
  std::mt19937 random(991);std::normal_distribution<double> normal(0,1/std::sqrt(2.));
  double searched=0,fixed=0;
  for(unsigned trial=0;trial<128;++trial){
    noiseRefined.reset();noiseBaseline.reset();
    for(uint64_t sequence=1;sequence<=8;++sequence){
      const Experiment::Frame frame{3,sequence,first+(sequence-1)*8,first+sequence*8,509000000,1,80,true,true};
      map.data[1][0]=Complex(normal(random),normal(random));power.data[1][0]=std::norm(map.data[1][0]);
      const Experiment::Pilot pilot{frame,0,1.,1.};
      result=noiseRefined.process({&map},{0},frame,pilot,power);
      unrefined=noiseBaseline.process({&map},{0},frame,pilot,power);
    }
    const double a=std::norm(result.banks[2].map->data[1][0]),b=std::norm(unrefined.banks[2].map->data[1][0]);
    check(a+1e-12>=b,"Fractional phase bank dropped its zero-frequency hypothesis");searched+=a;fixed+=b;
  }
  check(searched>1.2*fixed,"Noise phase-search inflation control is insensitive");
}
void cachedPhaseParity(bool fractional=false,int direction=1) {
  using Engine=blah2::integration::CoherentPathEvidence;using Complex=std::complex<double>;
  Engine::Config config;config.delayMotionBins=1;config.dopplerMotionBins=1;
  config.delayDriftPerHz=-.02;config.minimumCoherence=.3;config.refineDopplerPhase=true;
  config.interpolateDelay=fractional;config.heterodyneCenterHz=3;
  config.phaseDirection=direction;
  const unsigned columns=fractional?41:17;
  Engine engine(config);Map<Complex> map(9,columns);map.delay.resize(columns);map.doppler.resize(9);
  for(unsigned c=0;c<columns;++c)map.delay[c]=c;
  for(unsigned r=0;r<9;++r)map.doppler[r]=(int(r)-4)*10.;
  std::mt19937 random(313);std::normal_distribution<double> normal;
  std::vector<std::vector<Complex>> history;std::array<Engine::Bank,3> banks;
  for(unsigned n=0;n<8;++n){
    history.emplace_back();for(auto& row:map.data)for(auto& z:row){z={normal(random),normal(random)};history.back().push_back(z);}
    banks=engine.process(map,1,n+1,100+n*8,80);
  }
  const double tau=2*std::acos(-1.);
  for(unsigned b=0;b<3;++b)for(unsigned r=0;r<9;++r)for(unsigned c=0;c<columns;++c){
    const auto depth=2u<<b;double best=-1;Complex expected{};
    for(int dr=-1;dr<=1;++dr)for(int dc=-1;dc<=1;++dc)for(unsigned trial=0;trial<2*depth;++trial){
      const int index=trial==0?0:trial<=depth?int(trial)-int(depth)-1:int(trial)-int(depth);
      const double offsetHz=double(index)/(2*depth*.1);
      Complex sum{};double power=0,cycles=0;bool usable=true;
      for(unsigned age=0;age<depth;++age){
        const int rr=int(r)-dr*int(age);if(rr<0||rr>=9){usable=false;break;}
        const int cc=track_before_detect::delayPathColumn(c,dc,age,config.delayDriftPerHz,map.doppler[r],map.doppler[rr]);
        if(cc<0||cc>=int(columns)){usable=false;break;}
        if(age)cycles+=.05*(map.doppler[rr+dr]+map.doppler[rr]);
        Complex z=history[7-age][rr*columns+cc];
        if(fractional){
          const double physical=-config.delayDriftPerHz*.5*(map.doppler[r]+map.doppler[rr])*age;
          const double shift=physical-dc*int(age),coordinate=c+shift;
          if(physical){
            if(coordinate<8||coordinate+8>=columns){usable=false;break;}
            z=blah2::integration::RangeMigrationReplay::sampleProfile(history[7-age],columns,rr,coordinate,3,80);
          }else z=history[7-age][rr*columns+int(c)-dc*int(age)];
          z*=std::polar(1.,-tau*3*shift/80);
        }
        power+=std::norm(z);
        sum+=z*std::polar(1.,direction*tau*(cycles+offsetHz*.1*age));
      }
      const double score=std::norm(sum)/depth;
      if(usable&&score/(power)>config.minimumCoherence&&score>best){best=score;expected=sum/std::sqrt(double(depth));}
    }
    const auto cell=r*columns+c;check(bool(banks[b].valid[cell])==(best>=0),"Cached path validity differs from scalar oracle");
    if(best>=0)check(std::abs(banks[b].map->data[r][c]-expected)<1e-11,"Cached fractional phase differs from independent scalar oracle");
  }
}
void phaseGridAndPowerControls() {
  using Engine=blah2::integration::CoherentPathEvidence;
  Engine::Config config;config.delayMotionBins=0;config.dopplerMotionBins=0;config.refineDopplerPhase=true;
  Map<std::complex<double>> map(1,1);map.delay={0};map.doppler={0};
  const double tau=2*std::acos(-1.);
  // Sweep all coarse bins AND half-between-bin worst cases. The grid is tied
  // to capture hop, including overlapping CPIs, not the fast-CAF bin spacing.
  std::array<double,3> minimumCoherence{1.,1.,1.};
  for(unsigned depth:{2u,4u,8u})for(unsigned index=0;index<4*depth;++index){
    config.maximumCpis=depth;Engine engine(config);std::array<Engine::Bank,3> result;
    const double hz=(double(index)-2*depth)/(4*depth*.1);
    for(unsigned n=0;n<depth;++n){map.data[0][0]=std::polar(1.,tau*hz*.1*n);result=engine.process(map,1,n+1,100+n*8,80,nullptr,120+n*8);}
    const auto& bank=result[depth==2?0:depth==4?1:2];
    minimumCoherence[depth==2?0:depth==4?1:2]=std::min(minimumCoherence[depth==2?0:depth==4?1:2],bank.coherence[0]);
    check(bank.phaseTrials==2*depth&&bank.valid[0]&&bank.coherence[0]>.81,
      "Twofold slow-time grid loses a half-bin echo or duplicates Nyquist");
    check(std::norm(bank.map->data[0][0])>double(depth)*.81,"Slow-time phase grid lost its worst-case gain bound");
    check(bank.lineageAt(0).endSamples[0]==120+(depth-1)*8,"Overlapping CPI ranges were lost by phase refinement");
  }
  // Noncoherent mean power retains signal/noise units; its benefit is lower
  // noise variance, not an invented N-fold reduction in mean noise power.
  blah2::integration::TemporalEvidence power({8,0,0});
  std::mt19937 random(575);std::normal_distribution<double> normal(0,1/std::sqrt(2.));
  std::array<double,4> sum{},squares{};constexpr unsigned trials=2048;
  for(unsigned trial=0;trial<trials;++trial){
    power.reset();blah2::integration::TemporalEvidence::Result result;
    for(unsigned n=0;n<8;++n){const std::complex<double> noise{normal(random),normal(random)};map.data[0][0]=std::norm(noise);result=power.process(map,1,n+1,nullptr,nullptr,blah2::integration::TemporalEvidence::Input::realPower);}
    const std::array<double,4> values{map.data[0][0].real(),result.banks[1].detectorPower[0],result.banks[2].detectorPower[0],result.banks[3].detectorPower[0]};
    for(unsigned b=0;b<4;++b){sum[b]+=values[b];squares[b]+=values[b]*values[b];}
  }
  for(unsigned b=0;b<4;++b){const double mean=sum[b]/trials,variance=squares[b]/trials-mean*mean;
    check(std::abs(mean-1)<.1&&std::abs(variance*double(1u<<b)-1)<.2,"Canonical noncoherent noise mean/variance changed");
    std::cout<<"noncoherent_noise depth="<<(1u<<b)<<" mean="<<mean<<" variance="<<variance<<'\n';}
  for(unsigned b=0;b<3;++b)std::cout<<"phase_grid depth="<<(2u<<b)<<" minimum_coherence="<<minimumCoherence[b]<<'\n';
}
void fractionalComplexRange() {
  using Engine=blah2::integration::CoherentPathEvidence;
  using Migration=blah2::integration::RangeMigrationReplay;
  const double tau=2*std::acos(-1.);
  for(double center:{0.,137.}) {
    Engine::Config config;config.delayMotionBins=0;config.dopplerMotionBins=0;
    config.delayDriftPerHz=-.01;config.interpolateDelay=true;config.heterodyneCenterHz=center;
    Engine engine(config);Map<std::complex<double>> map(1,64);map.delay.resize(64);map.doppler={20};
    for(unsigned c=0;c<64;++c)map.delay[c]=c;
    std::array<Engine::Bank,3> result;
    for(unsigned n=0;n<8;++n) {
      for(unsigned c=0;c<64;++c)map.data[0][c]=std::polar(1.,tau*(.125*(c+.2*n)+center*c/1000+20*.1*n));
      result=engine.process(map,1,n+1,100+n*100,1000,nullptr,200+n*100);
    }
    for(unsigned b=0;b<3;++b) {
      const auto depth=2u<<b;const auto& bank=result[b];double gain=1.;
      for(unsigned age=1;age<depth;++age)gain+=Migration::prepareProfile(.2*age,center,1000).whiteNoiseGain();
      check(bank.valid[32]&&bank.coherence[32]>.9999,"Fractional range path loses continuous echo phase");
      check(std::abs(bank.map->data[0][32]-std::sqrt(double(depth))*map.data[0][32])<.005,
        "Fractional range interpolation omits/doubles newest-coordinate center correction");
      check(std::abs(bank.whiteRangeNoiseGain[32]-gain/depth)<1e-12,
        "Fractional range path lost interpolation noise-gain lineage");
      check(!bank.valid[0]&&!bank.valid[63]&&bank.lineageAt(32).firstSamples[depth-1]==100+(8-depth)*100,
        "Fractional range path pads missing support or loses raw ranges");
    }
    config.beamCount=2;rejects([&]{Engine refused(config);},"Fractional range mixes selected-beam identities");
  }
}

void sourceDomainPaths() {
  using NC = blah2::integration::TemporalEvidence;
  using Coherent = blah2::integration::CoherentPathEvidence;
  NC::Config nc; nc.delayMotionBins=1; nc.dopplerMotionBins=1;
  auto maskedNc=nc; maskedNc.sourceDomain={true,5,15};
  Coherent::Config coherent; coherent.delayMotionBins=1; coherent.dopplerMotionBins=1;
  coherent.minimumCoherence=0; coherent.refineDopplerPhase=true; coherent.cpiHopSeconds=.1;
  auto maskedCoherent=coherent; maskedCoherent.sourceDomain=maskedNc.sourceDomain;
  NC baseline(nc), bounded(maskedNc); Coherent original(coherent), boundedCoherent(maskedCoherent);
  Map<std::complex<double>> map(65,32);
  map.delay.clear(); map.doppler.clear();
  for(int c=0;c<32;++c)map.delay.push_back(c);
  for(int r=0;r<65;++r)map.doppler.push_back((r-32)*10.);
  for(auto& row:map.data)std::fill(row.begin(),row.end(),std::complex<double>{});
  map.data[32][12]=100; // Stationary clutter at excluded zero Doppler.
  map.data[36][0]=100;  // Excluded direct-path delay, searchable Doppler.
  map.data[40][24]=3;   // Independent in-domain stationary echo at80Hz.
  for(uint64_t sequence=1;sequence<=8;++sequence) {
    auto raw=baseline.process(map,1,sequence), safe=bounded.process(map,1,sequence);
    auto c=original.process(map,1,sequence,(sequence-1)*100,1000);
    auto s=boundedCoherent.process(map,1,sequence,(sequence-1)*100,1000);
    if(sequence!=4&&sequence!=8)continue;
    const size_t b=sequence==4?1:2, cell=34*32+12; // newest20Hz
    const auto path=baseline.auditPath(cell,uint32_t(sequence),1,0,false);
    check(path.valid&&path.mean==10000./sequence,
      "Zero-Doppler control no longer traverses20,10,0,-10Hz");
    check(!bounded.auditPath(cell,uint32_t(sequence),1,0,false).valid,
      "Historical excluded Doppler entered bounded NC path");
    check(raw.banks[b+1].detectorPower[cell]>=10000./sequence&&
      c[b].valid[cell]&&std::norm(c[b].map->data[34][12])>=10000./sequence-1e-9,
      "Default control unexpectedly changed canonical source-path eligibility");
    check(safe.banks[b+1].detectorPower[cell]==0&&!s[b].valid[cell],
      "Source-domain ablation pulled excluded clutter into a searchable output");
    const size_t echo=40*32+24;
    check(safe.banks[b+1].detectorPower[echo]==9&&s[b].valid[echo]&&
      std::abs(std::norm(s[b].map->data[40][24])-9*sequence)<1e-10,
      "Source-domain ablation removed valid stationary echo integration");
    if(sequence==8)check(raw.banks[3].detectorPower[36*32+7]>1000&&
      safe.banks[3].detectorPower[36*32+7]==0,
      "Historical excluded delay entered bounded NC path");
  }
  // Fractional kernels must not use excluded delay taps, even when their
  // center lies inside the domain. The opt-in domain validates explicit axes.
  maskedCoherent.interpolateDelay=true; maskedCoherent.delayDriftPerHz=-.01;
  maskedCoherent.delayMotionBins=maskedCoherent.dopplerMotionBins=0;
  auto unmaskedCoherent=maskedCoherent; unmaskedCoherent.sourceDomain.enabled=false;
  Coherent fractional(maskedCoherent), unmaskedFractional(unmaskedCoherent);
  std::fill(map.data[40].begin(),map.data[40].end(),std::complex<double>{1,0});
  for(uint64_t sequence=1;sequence<=2;++sequence) {
    auto result=fractional.process(map,1,sequence,(sequence-1)*100,1000);
    auto control=unmaskedFractional.process(map,1,sequence,(sequence-1)*100,1000);
    if(sequence==2)check(control[0].valid[40*32+8]&&!result[0].valid[40*32+8]&&result[0].valid[40*32+16],
      "Fractional source support crossed the excluded delay boundary");
  }
  auto invalid=map; invalid.delay={0,31}; NC refused(maskedNc);
  rejects([&]{refused.process(invalid,1,1);},"Source-domain ablation accepted implicit delay axes");
}
}

int main() {
  try {
    const auto value=config(); auto first=rr::test::fixture(value,1,{1,101,1001}); auto second=rr::test::fixture(value,1,{2,102,1002}); auto third=rr::test::fixture(value,1,{3,103,1003}); populatePilot(first); populatePilot(second); populatePilot(third);
    {
      auto& power=first.blocks.at(uint32_t(rr::BlockKind::currentPower)-1).bytes;
      blah2::resident::wire::Writer{{power.data(),power.size()}}.f64(0,9.25);
    }
    const auto firstBytes=rr::encode(first,value), secondBytes=rr::encode(second,value), thirdBytes=rr::encode(third,value); const auto firstSidecar=sidecar(first,value,100);
    const auto encoded=cr::encodeSidecar(firstSidecar,firstBytes.data(),firstBytes.size());
    check(cr::decodeSidecar(encoded.data(),encoded.size(),firstBytes.data(),firstBytes.size()).firstSample==100, "sidecar round trip differs");
    const auto zeroSource=sidecar(first,value,0,0);
    const auto zeroEncoded=cr::encodeSidecar(zeroSource,firstBytes.data(),firstBytes.size());
    const auto zeroDecoded=cr::decodeSidecar(zeroEncoded.data(),zeroEncoded.size(),firstBytes.data(),firstBytes.size());
    check(zeroDecoded.captureEpoch==0&&zeroDecoded.firstSample==0&&
      zeroDecoded.identity.epoch==first.identity.epoch,
      "Source epoch zero was rejected or confused with resident session identity");
    auto missingSession=zeroSource;missingSession.identity.epoch=0;
    rejects([&]{cr::encodeSidecar(missingSession,firstBytes.data(),firstBytes.size());},
      "Missing resident session was accepted alongside valid source epoch zero");
    auto tampered=encoded; tampered[120]^=1; rejects([&]{cr::decodeSidecar(tampered.data(),tampered.size(),firstBytes.data(),firstBytes.size());}, "tampered sidecar accepted");
    rejects([&]{cr::decodeSidecar(encoded.data(),encoded.size(),secondBytes.data(),secondBytes.size());}, "sidecar snapshot swap accepted");
    const auto directory=fs::temp_directory_path()/"direct-path-coherent-replay-contract"; std::error_code ignored; fs::remove_all(directory,ignored); fs::create_directory(directory);
    fractionalMaps(directory);
    hybridReplay(directory);
    composedMigration(directory);
    overlappingFractionalReader(directory);fractionalPhase();cachedPhaseParity();cachedPhaseParity(true);cachedPhaseParity(true,-1);phaseGridAndPowerControls();fractionalComplexRange();sourceDomainPaths();
    const auto one=directory/"one.vwrps", two=directory/"two.vwrps", three=directory/"three.vwrps"; write(one,firstBytes); write(two,secondBytes); write(three,thirdBytes);
    rejects([&]{cr::readSidecar(one);}, "missing sidecar accepted");
    cr::writeSidecarExclusive(one,firstSidecar);
    const auto validated = cr::loadValidatedSnapshot(one);
    check(validated.sidecar.identity.logicalFrame == first.identity.logicalFrame &&
      validated.sidecar.identity.rawIqHash == first.identity.rawIqHash &&
      validated.config.geometry.clutterSamples == value.geometry.clutterSamples &&
      validated.snapshot.blocks.at(uint32_t(rr::BlockKind::rawIq)-1).bytes ==
        first.blocks.at(uint32_t(rr::BlockKind::rawIq)-1).bytes,
      "Validated snapshot parser changed sealed IQ/configuration data");
    // Validly encoded but incompatible metadata must fail before experiment input.
    auto changedTune=sidecar(second,value,164,3,538000000); cr::writeSidecarExclusive(two,changedTune);
    cr::ExperimentReader reader(experimentConfig()); const auto fallback=reader.append(one); check(fallback.powerFallback != nullptr, "fallback map missing");
    check(fallback.powerFallback->data[0][0] == blah2::integration::DirectPathCoherentExperiment::Complex(9.25,0),
      "fallback did not retain accepted current-power map");
    rejects([&]{reader.append(two);}, "changed tuning sidecar accepted");
    fs::remove(cr::sidecarPath(two)); auto changedGain=sidecar(second,value,164); changedGain.actualGainDb[0] += 1; cr::writeSidecarExclusive(two,changedGain);
    rejects([&]{reader.append(two);}, "changed gain sidecar accepted");
    fs::remove(cr::sidecarPath(two)); auto changedEpoch=sidecar(second,value,164,4); cr::writeSidecarExclusive(two,changedEpoch);
    rejects([&]{reader.append(two);}, "changed capture epoch sidecar accepted");
    fs::remove(cr::sidecarPath(two)); cr::writeSidecarExclusive(two,sidecar(second,value,164));
    const auto paired = reader.append(two);
    check(paired.hybridAt(0,0).valid && !paired.hybridAt(0,0).coherent &&
      paired.hybridAt(0,0).power == 4.625,
      "Hybrid fallback squared the recorded current-power map");
    auto changedHop=sidecar(third,value,356); cr::writeSidecarExclusive(three,changedHop);
    rejects([&]{reader.append(three);}, "Changed hop silently rebound immutable replay drift");
    fs::remove(cr::sidecarPath(three));cr::writeSidecarExclusive(three,sidecar(third,value,228));
    check(reader.append(three).hybridAt(0,0).valid,"Rejected hop poisoned the retained valid history");
    // A reordered logical frame is also rejected, independently of filenames.
    fs::remove(cr::sidecarPath(two)); auto reordered=sidecar(second,value,164); reordered.identity.logicalFrame=1; cr::writeSidecarExclusive(two,reordered);
    rejects([&]{reader.append(two);}, "reordered sidecar accepted");
    // A complete, internally valid frame from a different build/configuration
    // cannot bridge coherent history merely because RF/capture fields match.
    fs::remove(cr::sidecarPath(two)); auto changedModule=sidecar(second,value,164);
    changedModule.identity.moduleHash[0]^=1; cr::writeSidecarExclusive(two,changedModule);
    rejects([&]{reader.append(two);}, "changed module provenance bridged coherent history");
    fs::remove_all(directory,ignored);
    std::cout << "COMPLETE direct-path coherent replay CPU contract gpu_execution=0 detector_mutation=0\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL direct-path coherent replay: " << error.what() << '\n'; return 1; }
}
