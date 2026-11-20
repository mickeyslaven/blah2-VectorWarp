#include "process/integration/DirectPathCoherentExperiment.h"
#include "ResidentReplayFixture.h"

#include <filesystem>
#include <fstream>
#include <iostream>

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
  value.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,4,true);
  value.detector.integrationBeamTolerance=UINT32_MAX;
  value.fusion={blah2::resident::FusionMode::coherentRegularPolygon,12,539000000,.2,0,-1.2566370614359172,-.75,6};
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
    fs::remove(cr::sidecarPath(two)); cr::writeSidecarExclusive(two,sidecar(second,value,164)); reader.append(two);
    auto changedHop=sidecar(third,value,356); cr::writeSidecarExclusive(three,changedHop);
    const auto hopFallback=reader.append(three); check(hopFallback.powerFallback != nullptr && hopFallback.reason == "lineage_reset", "changed hop did not fall back");
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
