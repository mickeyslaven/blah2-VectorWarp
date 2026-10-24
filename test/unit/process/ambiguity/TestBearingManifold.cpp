#include "process/ambiguity/BearingManifold.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace
{
blah2::bearing_manifold::Identity identity()
{
  return {{0,1},{14,14},{{{0,0,0}},{{.1,0,0}}},{40,-80,300},{40.1,-79.9,400},
    563000000,2400000,80,90};
}
class ManifoldFile
{
public:
  explicit ManifoldFile(const char* contents)
    : path_(std::filesystem::temp_directory_path() /
        "vectorwarp-bearing-manifold-test.json")
  {
    std::ofstream output(path_);
    output << contents;
  }
  ~ManifoldFile() { std::error_code error; std::filesystem::remove(path_, error); }
  const std::string path() const { return path_.string(); }
private:
  std::filesystem::path path_;
};
}

TEST_CASE("bearing manifold loader requires exact frequency beams and channels")
{
  ManifoldFile file(R"({"schema":"vectorwarp-bearing-manifold-v2",
    "frequency_hz":563000000,"identity":{"frequency_hz":563000000,
    "sample_rate_hz":2400000,"compatibility_fingerprint":"fnv1a64:5cf7bf5a92d347e1",
    "direct_path_local_azimuth_deg":90,"direct_path_beam":1,
    "channel_mapping":[0,1],"requested_gain_db":[14,14],"actual_gain_db":[14.2,14.1],
    "positions_m":[[0,0,0],[0.1,0,0]],"receiver_location":[40,-80,300],
    "transmitter_location":[40.1,-79.9,400],"x_axis_bearing_deg_true":80},
    "responses":[[[1,0],[0,1]],[[0.5,0.5],[1,-0.25]]]})");
  const auto loaded = blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,identity());
  REQUIRE(loaded.frequencyHz == 563000000);
  REQUIRE(loaded.responses.size() == 4);
  REQUIRE(loaded.directResponse == std::vector<std::complex<float>>{{.5f,.5f},{1,-.25f}});
  REQUIRE_NOTHROW(blah2::bearing_manifold::validateActualGains(loaded,{14.2,14.1}));
  REQUIRE_THROWS_AS(blah2::bearing_manifold::validateActualGains(loaded,{14.2,14.2}),
    std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::validateActualGains(loaded,{14.20001,14.1}),
    std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000001,2,2,identity()),
    std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,3,2,identity()),
    std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadDetection(file.path(),563000000,2,2,identity(),
    blah2::bearing_manifold::DetectionUse::offlineReplay),
    std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,3,identity()),
    std::invalid_argument);
  auto changed=identity();changed.positionsM[1][0]=.11;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.channelMapping={1,0};
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.requestedGainDb[0]=15;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.receiverLocation[1]=-80.1;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.transmitterLocation[0]=40.2;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.sampleRateHz=2000000;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
  changed=identity();changed.directPathAzimuthDeg=180;
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,2,2,changed),
    std::invalid_argument);
}

TEST_CASE("bearing manifold loader rejects zero responses")
{
  ManifoldFile file(R"({"schema":"vectorwarp-bearing-manifold-v2",
    "frequency_hz":563000000,"identity":{"frequency_hz":563000000,
    "sample_rate_hz":2400000,"compatibility_fingerprint":"fnv1a64:5cf7bf5a92d347e1",
    "direct_path_local_azimuth_deg":90,"direct_path_beam":0,
    "channel_mapping":[0,1],"requested_gain_db":[14,14],"actual_gain_db":[14.2,14.1],
    "positions_m":[[0,0,0],[0.1,0,0]],"receiver_location":[40,-80,300],
    "transmitter_location":[40.1,-79.9,400],"x_axis_bearing_deg_true":80},
    "responses":[[[1,0],[0,0]]]})");
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(file.path(),563000000,1,2,identity()),
    std::invalid_argument);
}

TEST_CASE("detection runtime loader preserves a separate direct anchor")
{
  ManifoldFile file(R"({"schema":"vectorwarp-detection-manifold-runtime-v1",
    "frequency_hz":563000000,"sample_rate_hz":2400000,"channels":2,"beams":2,
    "runtime_compatible":true,
    "intended_use":"offline-production-equivalent-replay-only",
    "channel_mapping":[0,1],
    "direct_path_local_azimuth_deg":90,"direct_path_beam":1,
    "manifold_identity":{"frequency_hz":563000000,"sample_rate_hz":2400000,
    "compatibility_fingerprint":"fnv1a64:5cf7bf5a92d347e1",
    "channel_mapping":[0,1],"requested_gain_db":[14,14],"actual_gain_db":[14.2,14.1],
    "positions_m":[[0,0,0],[0.1,0,0]],"receiver_location":[40,-80,300],
    "transmitter_location":[40.1,-79.9,400],"x_axis_bearing_deg_true":80},
    "direct_anchor":{"local_azimuth_deg":90,"beam":1,
      "response":[[0.123456789,-0.987654321],[0,1]]},
    "responses":[[[1,0],[1,0]],[[0.25,0.75],[-0.5,0.5]]]})");
  const auto loaded=blah2::bearing_manifold::loadDetection(file.path(),563000000,2,2,identity(),
    blah2::bearing_manifold::DetectionUse::offlineReplay);
  REQUIRE(loaded.directResponse==std::vector<std::complex<float>>{
    {float(.123456789),float(-.987654321)},{0,1}});
  REQUIRE(loaded.responses[2]!=loaded.directResponse[0]);
  REQUIRE(loaded.responses[3]!=loaded.directResponse[1]);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadBearing(
    file.path(),563000000,2,2,identity()),std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::bearing_manifold::loadDetection(
    file.path(),563000000,2,2,identity(),
    blah2::bearing_manifold::DetectionUse::liveProduction),std::invalid_argument);
}
