/// @file TestTracker.cpp
/// @brief Unit test for Tracker.cpp
/// @author 30hours

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "data/Detection.h"
#include "data/Track.h"
#include "process/tracker/Tracker.h"
#include "data/meta/Constants.h"
#include "rapidjson/document.h"

#include <string>
#include <vector>
#include <random>
#include <iostream>

/// @brief Test constructor.
/// @details Check constructor parameters created correctly.
TEST_CASE("Constructor", "[constructor]")
{
  uint32_t m = 3;
  uint32_t n = 5;
  uint32_t nDelete = 5;
  double cpi = 1;
  double maxAccInit = 10;
  double fs = 2000000;
  double rangeRes = (double)Constants::c/fs;
  double fc = 204640000;
  double lambda = (double)Constants::c/fc;
  Tracker tracker = Tracker(m, n, nDelete, 
    cpi, maxAccInit, rangeRes, lambda);
}

/// @brief Test process for an ACTIVE track.
TEST_CASE("Process ACTIVE track constant acc", "[process]")
{
  uint32_t m = 3;
  uint32_t n = 5;
  uint32_t nDelete = 5;
  double cpi = 1;
  double maxAccInit = 10;
  double fs = 2000000;
  double rangeRes = (double)Constants::c/fs;
  double fc = 204640000;
  double lambda = (double)Constants::c/fc;
  Tracker tracker = Tracker(m, n, nDelete, 
    cpi, maxAccInit, rangeRes, lambda);
  
  
  // create detections with constant acc 5 Hz/s
  std::vector<uint64_t> timestamp = {0,1,2,3,4,5,6,7,8,9,10};
  std::vector<double> delay = {10};
  std::vector<double> doppler = {-20,-15,-10,-5,0,5,10,15,20,25};

  std::string state = "ACTIVE";
}

/// @brief Test predict for kinematics equations.
TEST_CASE("Test predict", "[predict]")
{
  uint32_t m = 3;
  uint32_t n = 5;
  uint32_t nDelete = 5;
  double cpi = 1;
  double maxAccInit = 10;
  double fs = 2000000;
  double rangeRes = (double)Constants::c/fs;
  double fc = 204640000;
  double lambda = (double)Constants::c/fc;
  Tracker tracker = Tracker(m, n, nDelete, 
    cpi, maxAccInit, rangeRes, lambda);

  Detection input = Detection(10, -20, 0);
  double acc = 5;
  double T = 1;
  Detection prediction = tracker.predict(input, acc, T);
  Detection prediction_truth = Detection(10 + 17.5 * lambda / rangeRes, -15, 0);

  CHECK_THAT(prediction.get_delay().front(), 
    Catch::Matchers::WithinAbs(prediction_truth.get_delay().front(), 0.01));
  CHECK_THAT(prediction.get_doppler().front(), 
    Catch::Matchers::WithinAbs(prediction_truth.get_doppler().front(), 0.01));
}

TEST_CASE("Track histories are bounded without limiting M-of-N promotion", "[track]")
{
  Track track;
  const auto index = track.add(Detection(0, 0, 1));
  for (int i = 1; i <= 500; ++i) {
    track.set_current(index, Detection(i, -i, 1));
    track.set_state(index, "ASSOCIATED");
  }
  track.promote(index, 250, 255);
  CHECK(track.get_state(index) == "ACTIVE");
  CHECK(track.get_nAssociated(index) == 501);
  rapidjson::Document doc;
  doc.Parse(track.to_json(0).c_str());
  REQUIRE_FALSE(doc.HasParseError());
  const auto& data = doc["data"][0];
  CHECK(data["n"].GetUint64() == 501);
  CHECK(data["associated_delay"].Size() == 100);
  CHECK(data["associated_doppler"].Size() == 100);
  CHECK(data["associated_state"].Size() == 100);
  CHECK(data["associated_delay"][0].GetDouble() == 401);
  CHECK(data["associated_delay"][99].GetDouble() == 500);
  CHECK(std::string(data["associated_state"][99].GetString()) == "ACTIVE");
}

TEST_CASE("Removing a track keeps per-track counters aligned", "[track]")
{
  Track track;
  track.add(Detection(1, 1, 1));
  track.add(Detection(2, 2, 2));
  track.set_nInactive(0, 5);
  track.set_nInactive(1, 1);
  track.set_current(1, Detection(3, 3, 3));
  track.set_state(1, "ASSOCIATED");
  track.remove(0);
  CHECK(track.get_n() == 1);
  CHECK(track.get_nInactive(0) == 1);
  CHECK(track.get_nAssociated(0) == 2);
  CHECK(track.get_current(0).get_delay().front() == 3);
}

TEST_CASE("Association follows predicted position and preserves the measurement", "[tracker]")
{
  Tracker tracker(2, 3, 1, 1, 0, 100, 2);
  Detection first(20, -10, 5);
  tracker.process(&first, 1000);
  Detection second(20.25, -10.2, 8);
  auto tracks = tracker.process(&second, 2000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nInactive(0) == 0);
  CHECK(tracks->get_current(0).get_delay().front() == 20.25);
  CHECK(tracks->get_nAssociated(0) == 2);
  Detection third(20.5, -10.4, 8);
  tracks = tracker.process(&third, 3000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_state(0) == "ACTIVE");
  Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
  tracks = tracker.process(&empty, 4000);
  CHECK(tracks->get_state(0) == "COASTING");
  CHECK(tracks->get_nInactive(0) == 1);
  Detection reacquired(20.95, -10.8, 8);
  tracks = tracker.process(&reacquired, 5000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_state(0) == "ACTIVE");
  CHECK(tracks->get_nInactive(0) == 0);
  tracks = tracker.process(&reacquired, 5000);
  CHECK(tracks->get_nAssociated(0) == 5);
  tracks = tracker.process(&reacquired, 4999);
  CHECK(tracks->get_nAssociated(0) == 5);
  tracker.process(&empty, 6000);
  tracks = tracker.process(&empty, 7000);
  CHECK(tracks->get_n() == 0);
}

TEST_CASE("Multiple stale tracks are removed without skipping or corrupting indices", "[tracker]")
{
  Tracker tracker(1, 2, 0, 1, 0, 100, 2);
  Detection first({10, 50, 90}, {-10, 10, 30}, {1, 1, 1});
  REQUIRE(tracker.process(&first, 1000)->get_n() == 3);
  Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
  CHECK(tracker.process(&empty, 2000)->get_n() == 0);
  CHECK(tracker.process(&first, 3000)->get_n() == 3);
}

TEST_CASE("One detection cannot update two tracks", "[tracker]")
{
  Tracker tracker(1, 2, 5, 1, 0, 100, 2);
  Detection first({20, 20.5}, {-10, -10}, {1, 1});
  tracker.process(&first, 1000);
  Detection next(20.3, -10, 2);
  const auto tracks = tracker.process(&next, 2000);
  REQUIRE(tracks->get_n() == 2);
  CHECK(tracks->get_nState("ACTIVE") == 1);
  CHECK(tracks->get_nInactive(0) + tracks->get_nInactive(1) == 1);
}
