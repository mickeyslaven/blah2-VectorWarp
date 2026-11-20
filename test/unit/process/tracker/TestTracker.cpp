/// @file TestTracker.cpp
/// @brief Unit test for Tracker.cpp
/// @author 30hours

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "data/Detection.h"
#include "data/Track.h"
#include "process/tracker/Tracker.h"
#include "process/tracker/TrackerDiagnosticsJson.h"
#include "process/ambiguity/GpuResidentQualification.h"
#include "data/meta/Constants.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include <string>
#include <vector>
#include <random>
#include <iostream>

TEST_CASE("CPU tracker diagnostics JSON reports exclusive radar-only frame counters", "[tracker][json]")
{
  Tracker tracker(2, 3, 3, .5, 10, 50, .6, 7, true);
  Detection first(100, 100, 10);
  tracker.process(&first, 1000);
  Detection next({99.4, 130, 99.4, 99.4}, {100, 100, 120, 100}, {10, 5, 5, 5});
  tracker.process(&next, 1500);
  const auto& counters = tracker.frame_diagnostics();
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writeTrackerFrameDiagnostics(writer, counters);
  rapidjson::Document json;
  json.Parse(buffer.GetString());
  REQUIRE_FALSE(json.HasParseError());
  CHECK(std::string(json["schema"].GetString()) == "vectorwarp-cpu-tracker-frame-diagnostics-v1");
  CHECK(json["truth_blind"].GetBool());
  CHECK(json["detections"].GetUint64() == 4);
  CHECK(json["pair_innovation_rejected"].GetUint64() == 0);
  CHECK(json["pair_delay_rejected"].GetUint64() == 1);
  CHECK(json["pair_doppler_rejected"].GetUint64() == 1);
  CHECK(json["pair_in_gate"].GetUint64() == 2);
  CHECK(json["associations"].GetUint64() == 1);
  CHECK(json["birth_snr_rejected"].GetUint64() == 3);
  CHECK_FALSE(json.HasMember("birth_exclusive_rejected"));
  CHECK(json["promotions"].GetUint64() == 0); // unresolved same-delay Doppler pair
  CHECK(json["candidate_track_pairs"].GetUint64() ==
    json["pair_already_claimed"].GetUint64() + json["pair_delay_rejected"].GetUint64() +
    json["pair_doppler_rejected"].GetUint64() + json["pair_innovation_rejected"].GetUint64() +
    json["pair_in_gate"].GetUint64());
  tracker.process(&next, 1500);
  CHECK(tracker.frame_diagnostics().noOp);
  CHECK(tracker.frame_diagnostics().associations == 0);
  CHECK(tracker.frame_diagnostics().promotions == 0);
}

TEST_CASE("T01 canonical comb births retain strength and ordered ties", "[tracker][admission]")
{
  for(bool adaptive:{false,true})for(bool range:{false,true})for(bool ties:{false,true}) {
    std::vector<unsigned> order{0,1,2};
    do {
      std::vector<double> delay,doppler,snr;
      for(auto i:order){delay.push_back(range?20:8+12*i);doppler.push_back(range?-16+16*int(i):2*int(i));snr.push_back(ties?10:8+2*i);}
      Tracker tracker(1,4,4,.5,0,100000,1,5,adaptive);
      Detection input(delay,doppler,snr);const auto tracks=tracker.process(&input,1000);
      REQUIRE(tracks->get_n()==1);
      const auto winner=ties?0:std::find(order.begin(),order.end(),2)-order.begin();
      CHECK(tracks->get_current(0).get_delay().front()==delay[winner]);
      CHECK(tracks->get_current(0).get_doppler().front()==doppler[winner]);
      CHECK(tracks->get_state(0)==(range?"TENTATIVE":"ACTIVE"));
      CHECK(tracker.frame_diagnostics().birthDopplerCombRejected==uint64_t(range?0:2));
      CHECK(tracker.frame_diagnostics().birthRangeCombRejected==uint64_t(range?2:0));
    }while(std::next_permutation(order.begin(),order.end()));
  }
  Tracker control(1,4,4,.5,0,100000,1,5,true);
  Detection independent({8,20,34},{-20,0,20},{10,10,10});
  CHECK(control.process(&independent,1000)->get_nState("ACTIVE")==3);
}

TEST_CASE("T02 range ambiguity retains measured votes without premature promotion", "[tracker][admission]")
{
  for(uint32_t minimum:{1u,2u}) {
    Tracker tracker(minimum,3,3,.5,0,100000,1,5,true);
    Detection ambiguous({20,20},{-8,8},{10,10});
    for(uint64_t frame=1;frame<=2;++frame) {
      auto tracks=tracker.process(&ambiguous,500*frame);
      REQUIRE(tracks->get_n()==2);CHECK(tracks->get_nState("ACTIVE")==0);
    }
    Detection resolved(20,-8,10);
    auto tracks=tracker.process(&resolved,1500);
    REQUIRE(tracks->get_n()==1);CHECK(tracks->get_state(0)=="ACTIVE");
    Detection none(std::vector<double>{},std::vector<double>{},std::vector<double>{});
    CHECK(tracker.process(&none,2000)->get_state(0)=="COASTING");
    for(uint64_t frame=5;frame<=7;++frame)tracks=tracker.process(&none,500*frame);
    CHECK(tracks->get_n()==0);
  }
}

TEST_CASE("T03 canonical tracker accepts first interior and final signed-grid columns", "[tracker][admission]")
{
  for(double delay:{-4.,0.,4.})for(double doppler:{-2.,0.,2.}) {
    Tracker tracker(1,3,3,.5,0,100000,1,5,true);
    tracker.configureGrid({-4,-3,-2,-1,0,1,2,3,4},{-2,0,2});
    Detection input(delay,doppler,10);
    CHECK(tracker.process(&input,1000)->get_nState("ACTIVE")==1);
    CHECK(tracker.process(&input,1500)->get_n()==1);
    CHECK(tracker.frame_diagnostics().associations==1);
  }
}

TEST_CASE("T05 finite stationary cutoff is inclusive and permits measured recovery", "[tracker][stationary]")
{
  for(double sign:{-1.,1.})for(double outside:{std::nextafter(2.,3.),2.01})for(double maximum:{0.,2.}) {
    TrackerRecoveryConfig recovery{true,3,maximum,65536};
    Tracker tracker(2,6,4,.5,0,100000,1,5,true,{},true,9.210340371976184,{},recovery);
    Detection boundary(20,sign*2,10);
    for(uint64_t frame=1;frame<=4;++frame)tracker.process(&boundary,500*frame);
    CHECK(tracker.frame_diagnostics().stationaryQuarantined==1);
    Detection next(20,sign*outside,10);const auto tracks=tracker.process(&next,2500);
    REQUIRE(tracks->get_n()==1);
    CHECK(tracker.frame_diagnostics().stationaryQuarantined==uint64_t(maximum==0));
    CHECK(tracks->get_state(0)==(maximum==0?"TENTATIVE":"ACTIVE"));
  }
}

TEST_CASE("Measured seed counts toward early M-of-N confirmation", "[tracker][confirmation]")
{
  constexpr double rangeRes = 299792458.0 / 6144000.0;
  constexpr double lambda = 299792458.0 / 509000000.0;
  for (unsigned mask = 1; mask < 64; mask += 2) {
    Tracker tracker(4, 6, 3, .5, 10, rangeRes, lambda, 7, true);
    unsigned hits = 0;
    bool confirmed = false;
    for (unsigned frame = 0; frame < 6; ++frame) {
      const bool hit = (mask & (1u << frame)) != 0;
      hits += hit;
      Detection detection = hit ? Detection(100-lambda*100*.5*frame/rangeRes, 100, 10) :
        Detection(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
      auto tracks = tracker.process(&detection, 1000 + 500*frame);
      for (unsigned index = 0; index < tracks->get_n(); ++index) {
        const auto state = tracks->get_state(index);
        if (state == "ACTIVE" || state == "COASTING") {
          CHECK(hits >= 4);
          confirmed = true;
        } else CHECK(tracks->get_nAssociated(index) < 6);
      }
      if (mask == 63 && frame == 3) {
        REQUIRE(tracks->get_n() == 1);
        CHECK(tracks->get_state(0) == "ACTIVE");
      }
      const auto& d = tracker.frame_diagnostics();
      CHECK(d.pairs == d.pairClaimed + d.pairDelayRejected + d.pairDopplerRejected +
        d.pairInnovationRejected + d.pairInGate);
      CHECK(d.tracksAfter == d.tracksBefore - d.tentativeExpired - d.inactivityDeleted + d.initiated);
    }
    CHECK(confirmed == (hits >= 4));
  }
}

TEST_CASE("Tentative lifetime is bounded despite recurring sparse detections", "[tracker][confirmation]")
{
  Tracker tracker(4, 6, 3, .5, 10, 50, .6, 7, true);
  unsigned expiries = 0;
  for (unsigned frame = 0; frame < 60; ++frame) {
    Detection detection = frame % 3 == 0 ? Detection(100-.6*100*.5*frame/50, 100, 10) :
      Detection(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
    auto tracks = tracker.process(&detection, 1000 + 500*frame);
    expiries += tracker.frame_diagnostics().tentativeExpired;
    for (unsigned index = 0; index < tracks->get_n(); ++index) {
      CHECK(tracks->get_state(index) != "ACTIVE");
      CHECK(tracks->get_nAssociated(index) < 6);
    }
  }
  CHECK(expiries == 10);
}

TEST_CASE("First adaptive reassociation uses actual elapsed missed time", "[tracker][adaptive]")
{
  constexpr double rangeRes = 299792458.0 / 6144000.0;
  constexpr double lambda = 299792458.0 / 509000000.0;
  for (bool uneven : {false, true}) {
    Tracker tracker(4, 6, 3, .5, 10, rangeRes, lambda, 7, true);
    Detection first(100, 100, 10);
    tracker.process(&first, 1000);
    Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
    tracker.process(&empty, uneven ? 1300 : 1500);
    const double elapsed = uneven ? 1.1 : 1.;
    // maxAcc=10 at CPI=.5 has the canonical quantized span 8 Hz/s.
    Detection next(100-lambda*(100*elapsed+4*elapsed*elapsed)/rangeRes, 100+8*elapsed, 10);
    auto tracks = tracker.process(&next, uneven ? 2100 : 2000);
    REQUIRE(tracks->get_n() == 1);
    CHECK(tracks->get_nInactive(0) == 0);
    CHECK_THAT(tracks->get_acceleration(0), Catch::Matchers::WithinAbs(8, 1e-9));
  }
  Tracker rejected(4, 6, 3, .5, 10, rangeRes, lambda, 7, true);
  Detection first(100, 100, 10);
  rejected.process(&first, 1000);
  Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
  rejected.process(&empty, 1500);
  Detection outside(100-lambda*106.5/rangeRes, 113, 10);
  auto tracks = rejected.process(&outside, 2000);
  REQUIRE(tracks->get_n() == 2);
  CHECK(tracks->get_nInactive(0) == 2);
  CHECK(rejected.frame_diagnostics().pairDopplerRejected == 1);
}

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

TEST_CASE("Adaptive initiation creates one candidate per detection", "[tracker][adaptive]")
{
  constexpr double cpi = .5;
  constexpr double rangeRes = 299792458.0 / 6144000.0;
  constexpr double lambda = 299792458.0 / 527000000.0;
  Detection detection(10, 100, 8);
  Tracker hypotheses(4, 6, 3, cpi, 10, rangeRes, lambda, 5, false);
  Tracker adaptive(4, 6, 3, cpi, 10, rangeRes, lambda, 5, true);
  CHECK(hypotheses.process(&detection, 1000)->get_n() == 5);
  CHECK(adaptive.process(&detection, 1000)->get_n() == 1);
}

TEST_CASE("Adaptive gate retains a target through its first acceleration update",
  "[tracker][adaptive]")
{
  constexpr double cpi = .5;
  constexpr double rangeRes = 299792458.0 / 6144000.0;
  constexpr double lambda = 299792458.0 / 527000000.0;
  Tracker adaptive(4, 6, 3, cpi, 10, rangeRes, lambda, 5, true);
  Detection first(10, 100, 8);
  auto initial = adaptive.process(&first, 1000);
  REQUIRE(initial->get_n() == 1);
  Detection accelerated(10 - lambda * 102.5 * cpi / rangeRes, 105, 8);
  auto associated = adaptive.process(&accelerated, 1500);
  REQUIRE(associated->get_n() == 1);
  CHECK(associated->get_state(0) == "ASSOCIATED");
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

TEST_CASE("Persistent broad Doppler ridges block only new track births",
  "[tracker][doppler-ridge]")
{
  DopplerRidgeConfig ridge{true, 3, 2.0, .25};
  Tracker tracker(2, 2, 1, .5, 0, 50, 1, -100, true, ridge);
  for (uint64_t frame = 0; frame < 3; ++frame)
  {
    const double offset = double(frame) * 20;
    Detection detections({offset, offset + 10}, {120, 121}, {12, 12});
    const auto tracks = tracker.process(&detections, 1000 + frame * 500);
    CHECK(tracks->get_n() == (frame < 2 ? 2 : 0));
  }
}

TEST_CASE("An isolated moving return cannot teach a Doppler ridge",
  "[tracker][doppler-ridge]")
{
  DopplerRidgeConfig ridge{true, 2, 2.0, .25};
  Tracker tracker(2, 2, 1, .5, 0, 50, 1, -100, true, ridge);
  for (uint64_t frame = 0; frame < 4; ++frame)
  {
    Detection detection(20 + frame * 20, 120, 12);
    const auto tracks = tracker.process(&detection, 1000 + frame * 500);
    CHECK(tracks->get_n() == 1);
  }
}

TEST_CASE("Track histories are bounded without limiting M-of-N promotion", "[track]")
{
  Track track;
  const auto index = track.add(Detection(0, 0, 1));
  for (int i = 1; i <= 500; ++i) {
    track.set_state(index, "ASSOCIATED");
    track.set_current(index, Detection(i, -i, 1));
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
  track.set_state(1, "ASSOCIATED");
  track.set_current(1, Detection(3, 3, 3));
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
  CHECK(tracks->get_nAssociated(0) == 4);
  tracks = tracker.process(&reacquired, 4999);
  CHECK(tracks->get_nAssociated(0) == 4);
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

TEST_CASE("Weak detections cannot initiate conservative tracks", "[tracker]")
{
  Tracker tracker(1, 1, 3, 1, 0, 100, 2, 7);
  Detection weak(10, 20, 6.99);
  auto tracks = tracker.process(&weak, 1000);
  CHECK(tracks->get_n() == 0);
  Detection strong(10, 20, 7.0);
  tracks = tracker.process(&strong, 2000);
  CHECK(tracks->get_n() == 1);
  CHECK(tracks->get_state(0) == "ACTIVE");
}

namespace {
Detection::Evidence sourceEvidence(uint64_t epoch, uint64_t newest, uint8_t depth)
{
  if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
    throw std::invalid_argument("Test evidence depth is not canonical");
  Detection::Evidence value;
  value.epoch = epoch;
  value.newestSequence = newest;
  value.depth = depth;
  value.bank = depth <= 2 ? 1 : depth == 4 ? 2 : 3;
  value.delayMotion = 0;
  value.dopplerMotion = 0;
  for (uint8_t i = 0; i < depth; ++i) value.sequences[i] = newest - i;
  return value;
}

Detection temporalDetection(uint64_t epoch, uint64_t newest, uint8_t depth)
{
  Detection result(10, 0, 10);
  result.set_evidence({sourceEvidence(epoch, newest, depth)});
  return result;
}
}

TEST_CASE("Temporal detector windows receive one disjoint confirmation vote", "[tracker][lineage]")
{
  for(const uint8_t depth:{uint8_t(2),uint8_t(4),uint8_t(8)}) {
    Tracker tracker(2,2*depth+1,2*depth,1,0,100,2);
    auto seed=temporalDetection(0,depth,depth);
    auto tracks=tracker.process(&seed,1000*depth);
    REQUIRE(tracks->get_n()==1);
    CHECK(tracks->get_state(0)=="TENTATIVE");
    for(uint64_t sequence=depth+1;sequence<2*depth;++sequence) {
      auto overlap=temporalDetection(0,sequence,depth);
      tracks=tracker.process(&overlap,sequence*1000);
      REQUIRE(tracks->get_n()==1);
      CHECK(tracks->get_state(0)!="ACTIVE");
      CHECK(tracks->get_nAssociated(0)==1);
      CHECK(tracker.frame_diagnostics().pairLineageRejected==1);
      CHECK(tracker.frame_diagnostics().independentCredits==0);
    }
    auto independent=temporalDetection(0,2*depth,depth);
    tracks=tracker.process(&independent,2*depth*1000);
    REQUIRE(tracks->get_n()==1);
    CHECK(tracks->get_state(0)=="ACTIVE");
    CHECK(tracks->get_nAssociated(0)==2);
    CHECK(tracker.frame_diagnostics().creditedCpis==depth);
    tracker.process(&independent,(2*depth+1)*1000);
    CHECK(tracker.frame_diagnostics().noOp);
    auto changedEpoch=temporalDetection(1,depth,depth);
    tracks=tracker.process(&changedEpoch,(2*depth+2)*1000);
    REQUIRE(tracks->get_n()==1);
    CHECK(tracks->get_state(0)=="TENTATIVE");
    CHECK(tracker.frame_diagnostics().lineageResets==1);
    tracks=tracker.process(&seed,(2*depth+3)*1000);
    REQUIRE(tracks->get_n()==1);
    CHECK(tracker.frame_diagnostics().lineageResets==1); // Epoch zero is a real reset destination.
    tracker.reset();
    tracks=tracker.process(&seed,(2*depth+4)*1000);
    CHECK(tracks->get_nAssociated(0)==1);
  }
}

TEST_CASE("Private qualification budgets real confirmed tracks and resets", "[tracker][lineage][startup]")
{
  using blah2::resident::Qualification;
  for(const uint8_t depth:{uint8_t(1),uint8_t(2),uint8_t(4),uint8_t(8)})
    for(const uint32_t minimum:{1u,2u,3u,1u+254u/depth}) {
      const uint32_t window=tracker_confirmation::confirmationMinimumWindow(minimum,depth);
      const uint32_t due=Qualification::confirmationFrame(depth,minimum);
      Tracker tracker(minimum,window,depth-1,1,0,100,2);
      for(uint64_t epoch=0;epoch<2;++epoch) {
        tracker.reset();
        Qualification gate(Qualification::startupCount(8,depth,minimum,window,depth-1),depth);
        uint32_t frames=0;
        gate.prepareStartup([&](uint32_t frame){
          ++frames;
          // Hold the first candidate until its entire configured support is
          // positive. This prevents shallower/earlier hits masking the bound.
          const bool positive=frame>=depth,mature=frame+1>=2u*depth;
          Detection detection=mature?temporalDetection(epoch,frame+1,depth):
            Detection(std::vector<double>{},std::vector<double>{},std::vector<double>{});
          const auto tracks=tracker.process(&detection,1000ULL*(frame+1));
          rapidjson::Document product;product.Parse(tracks->to_json(frame+1).c_str());
          REQUIRE_FALSE(product.HasParseError());
          uint64_t confirmed=0;
          for(const auto& item:product["data"].GetArray()) {
            const std::string state=item["state"].GetString();
            if(state=="ACTIVE"||state=="COASTING")++confirmed;
          }
          CHECK(confirmed==uint64_t(frame+1>=due));
          if(frame+1==2u*depth&&minimum>1) {
            CHECK(product["n"].GetUint64()==1);
            CHECK(product["data"].Empty());
          }
          gate.control(positive,positive?mature:frame+1>=depth,
            detection.get_nDetections(),product["n"].GetUint64(),confirmed,true,frame+1>=due);
          gate.checked(Qualification::reference);gate.checked(Qualification::filtered);
          gate.checked(Qualification::maps);gate.checkedProducts(std::min(frame+1,uint32_t(depth)));
        },[]{},[&]{gate.requireControls();tracker.reset();});
        CHECK(frames==std::max(8u+depth-1,due));
        Detection empty(std::vector<double>{},std::vector<double>{},std::vector<double>{});
        CHECK(tracker.process(&empty,1000)->get_n()==0);
      }
    }
}

TEST_CASE("Mixed depth evidence shares one disjoint ledger", "[tracker][lineage]")
{
  Tracker tracker(3,20,20,1,0,100,2);
  auto seed=temporalDetection(0,2,2);tracker.process(&seed,2000);
  auto overlap4=temporalDetection(0,4,4);
  auto tracks=tracker.process(&overlap4,4000);
  CHECK(tracker.frame_diagnostics().pairLineageRejected==1);
  CHECK(tracks->get_nAssociated(0)==1);
  auto fresh4=temporalDetection(0,6,4);tracks=tracker.process(&fresh4,6000);
  CHECK(tracks->get_nAssociated(0)==2);
  auto overlap8=temporalDetection(0,10,8);tracks=tracker.process(&overlap8,10000);
  CHECK(tracker.frame_diagnostics().pairLineageRejected==1);
  CHECK(tracks->get_nAssociated(0)==2);
  auto fresh8=temporalDetection(0,14,8);tracks=tracker.process(&fresh8,14000);
  REQUIRE(tracks->get_n()==1);
  CHECK(tracks->get_state(0)=="ACTIVE");
  CHECK(tracks->get_nAssociated(0)==3);
  CHECK(tracker.frame_diagnostics().creditedCpis==8);
}

TEST_CASE("Explicit lineage reset reports the next frame once", "[tracker][lineage]")
{
  Tracker tracker(1,1,3,1,0,100,2);
  Detection value(10,0,10),none(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  tracker.process(&value,1000);
  tracker.reset(true);
  CHECK(tracker.process(&none,0)->get_n()==0);
  CHECK(tracker.frame_diagnostics().lineageResets==1);
  tracker.process(&none,0);
  CHECK(tracker.frame_diagnostics().noOp);
  CHECK(tracker.frame_diagnostics().lineageResets==0);
}

TEST_CASE("Missing or malformed lineage cannot bypass an initialized ledger", "[tracker][lineage]")
{
  Tracker tracker(2,8,8,1,0,100,2);
  auto seed=temporalDetection(0,2,2);
  tracker.process(&seed,2000);
  Detection stripped(10,0,10);
  auto tracks=tracker.process(&stripped,3000);
  REQUIRE(tracks->get_n()==1);
  CHECK(tracks->get_nAssociated(0)==1);
  CHECK(tracker.frame_diagnostics().pairLineageRejected==1);
  auto malformed=temporalDetection(1,3,2);
  auto evidence=sourceEvidence(1,3,2);evidence.depth=3;
  malformed.set_evidence({evidence});
  tracks=tracker.process(&malformed,4000);
  REQUIRE(tracks->get_n()==1);
  CHECK(tracker.frame_diagnostics().lineageResets==0);
  auto fresh=temporalDetection(0,4,2);
  tracks=tracker.process(&fresh,5000);
  REQUIRE(tracks->get_n()==1);
  CHECK(tracks->get_state(0)=="ACTIVE");
  CHECK(tracks->get_nAssociated(0)==2);
}

TEST_CASE("Disabled recovery preserves ordinary seed and M of N promotion", "[tracker][lineage]")
{
  Detection value(10,0,10);
  Tracker immediate(1,1,3,1,0,100,2);
  auto tracks=immediate.process(&value,1000);
  REQUIRE(tracks->get_n()==1);
  CHECK(tracks->get_state(0)=="ACTIVE");
  CHECK(tracks->get_nUpdates(0)==1);
  Tracker ordinary(2,4,3,1,0,100,2);
  tracks=ordinary.process(&value,1000);
  CHECK(tracks->get_state(0)=="TENTATIVE");
  tracks=ordinary.process(&value,2000);
  CHECK(tracks->get_state(0)=="ACTIVE");
  CHECK(tracks->get_nAssociated(0)==2);
  CHECK(ordinary.frame_diagnostics().independentCredits==0);
}

TEST_CASE("Malformed temporal lineage cannot create tracks", "[tracker][lineage]")
{
  for(const uint8_t depth:{uint8_t(0),uint8_t(3),uint8_t(9)}) {
    Tracker tracker(1,1,3,1,0,100,2);
    Detection value(10,0,10);
    auto evidence=sourceEvidence(0,8,2);evidence.depth=depth;
    value.set_evidence({evidence});
    CHECK(tracker.process(&value,1000)->get_n()==0);
    CHECK(tracker.frame_diagnostics().birthLineageRejected==1);
  }
  Tracker tracker(1,1,3,1,0,100,2);
  Detection value(10,0,10);
  auto evidence=sourceEvidence(0,8,4);evidence.sequences[2]=8;
  value.set_evidence({evidence});
  CHECK(tracker.process(&value,1000)->get_n()==0);
}

TEST_CASE("Shared stationary policy quarantines persistent cells and preserves distant targets", "[tracker][stationary]")
{
  TrackerRecoveryConfig config;config.stationaryClutter=true;config.persistenceCpis=2;
  config.maximumDopplerHz=2;config.maximumStationaryCells=128;
  Tracker tracker(1,3,4,1,0,100,2,-100,false,{},true,9.210340371976184,{},config);
  Detection stationary(10,0,10);
  REQUIRE(tracker.process(&stationary,1000)->get_nState("ACTIVE")==1);
  tracker.process(&stationary,2000);
  auto tracks=tracker.process(&stationary,3000);
  CHECK(tracks->get_nState("ACTIVE")==0);
  CHECK(tracker.frame_diagnostics().stationaryQuarantined==1);
  Detection mixture({10,100},{0,20},{10,10});
  tracks=tracker.process(&mixture,4000);
  CHECK(tracker.frame_diagnostics().stationaryQuarantined==1);
  CHECK(tracker.frame_diagnostics().stationaryBirthRejected==0);
  CHECK(tracks->get_nState("ACTIVE")==1);
  CHECK(tracker.frame_diagnostics().stationaryCells<=128);
  tracker.reset();
  CHECK(tracker.process(&stationary,5000)->get_nState("ACTIVE")==1);
}

TEST_CASE("Shared stationary occupancy has an explicit bounded capacity", "[tracker][stationary]")
{
  TrackerRecoveryConfig config;config.stationaryClutter=true;config.persistenceCpis=2;
  config.maximumStationaryCells=1;
  Tracker tracker(2,3,3,1,0,100,2,-100,false,{},true,9.210340371976184,{},config);
  Detection value({10,100},{0,20},{10,10});
  tracker.process(&value,1000);
  CHECK(tracker.frame_diagnostics().stationaryCells==1);
  CHECK(tracker.frame_diagnostics().stationaryCapacityRejected>0);
}

TEST_CASE("Stationary capacity pressure cannot quarantine established tracks", "[tracker][stationary]")
{
  TrackerRecoveryConfig config;config.stationaryClutter=true;config.persistenceCpis=2;
  config.maximumStationaryCells=54;
  Tracker tracker(1,5,5,1,0,100,2,-100,false,{},true,9.210340371976184,{},config);
  Detection targets({10,100},{0,20},{10,10});
  REQUIRE(tracker.process(&targets,1000)->get_nState("ACTIVE")==2);
  auto tracks=tracker.process(&targets,2000);
  REQUIRE(tracks->get_n()==2);
  CHECK(tracks->get_nState("ACTIVE")==2);
  CHECK(tracker.frame_diagnostics().associations==2);
  CHECK(tracker.frame_diagnostics().stationaryQuarantined==0);
  Detection distant(1000,0,10);
  tracker.process(&distant,3000);
  CHECK(tracker.frame_diagnostics().stationaryBirthRejected==0);
  CHECK(tracker.frame_diagnostics().stationaryBirthCapacityRejected==1);
}

TEST_CASE("Stationary decay frees admission capacity before processing the frame", "[tracker][stationary]")
{
  TrackerRecoveryConfig config;config.stationaryClutter=true;config.persistenceCpis=2;
  config.maximumStationaryCells=27;
  Tracker tracker(1,5,5,1,0,100,2,7,false,{},true,9.210340371976184,{},config);
  Detection weak(10,0,6),none(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  REQUIRE(tracker.process(&weak,1000)->get_n()==0);
  CHECK(tracker.frame_diagnostics().stationaryCells==27);
  for(uint64_t time=2000;time<=5000;time+=1000)tracker.process(&none,time);
  Detection distant(1000,0,10);
  CHECK(tracker.process(&distant,6000)->get_nState("ACTIVE")==1);
  CHECK(tracker.frame_diagnostics().stationaryBirthCapacityRejected==0);
}

TEST_CASE("Repeated empty-track timestamps cannot teach stationary occupancy", "[tracker][stationary]")
{
  TrackerRecoveryConfig config;config.stationaryClutter=true;config.persistenceCpis=2;
  Tracker tracker(1,5,5,1,0,100,2,7,false,{},true,9.210340371976184,{},config);
  Detection weak(10,0,6),strong(10,0,10);
  REQUIRE(tracker.process(&weak,1000)->get_n()==0);
  tracker.process(&weak,1000);
  CHECK(tracker.frame_diagnostics().noOp);
  CHECK(tracker.frame_diagnostics().stationaryCells==27);
  tracker.process(&weak,999);
  CHECK(tracker.frame_diagnostics().noOp);
  CHECK(tracker.process(&strong,2000)->get_n()==0);
  CHECK(tracker.frame_diagnostics().stationaryBirthRejected==1);
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

TEST_CASE("Established association uses the normalized resolution gate", "[tracker][association]")
{
  Tracker normalized(1, 2, 5, 1, 0, 100, 2, -100, false, {}, true);
  Tracker legacy(1, 2, 5, 1, 0, 100, 2, -100, false, {}, false);
  Detection seed(20, 0, 8);
  normalized.process(&seed, 1000);
  legacy.process(&seed, 1000);
  Detection twoDelayBinsAway(22, 0, 8);
  const auto normalizedTracks = normalized.process(&twoDelayBinsAway, 2000);
  const auto legacyTracks = legacy.process(&twoDelayBinsAway, 2000);
  REQUIRE(normalizedTracks->get_n() == 1);
  CHECK(normalizedTracks->get_nAssociated(0) == 2);
  CHECK(normalized.frame_diagnostics().associations == 1);
  REQUIRE(legacyTracks->get_n() == 2);
  CHECK(legacyTracks->get_nAssociated(0) == 1);
  CHECK(legacy.frame_diagnostics().pairDelayRejected == 1);
}

TEST_CASE("Adaptive gate widens only the first association", "[tracker]")
{
  Tracker tracker(2, 3, 5, 1, 5, 100, 2, -100, true);
  Detection first(20, 0, 8);
  tracker.process(&first, 1000);
  Detection estimating(20, 5, 8);
  auto tracks = tracker.process(&estimating, 2000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nAssociated(0) == 2);

  // The track now has an acceleration estimate. A measurement inside the
  // former wide envelope but outside the normal one-bin gate must not steal
  // the mature track; it starts an independent candidate instead.
  Detection outsideMatureGate(20, 15, 8);
  tracks = tracker.process(&outsideMatureGate, 3000);
  REQUIRE(tracks->get_n() == 2);
  CHECK(tracks->get_nAssociated(0) == 2);
  CHECK(tracks->get_nInactive(0) == 1);
  CHECK(tracks->get_current(0).get_doppler().front() == 10);
}

TEST_CASE("Initiation multiplicity shares exact floating point operation order", "[tracker]")
{
  CHECK(Tracker::initiationMultiplicity(.3, 11.11111111111111, false) == 3);
  CHECK(Tracker::initiationMultiplicity(.6, 2.7777777777777777, false) == 3);
  CHECK(Tracker::initiationMultiplicity(.5, 10, true) == 1);
}

TEST_CASE("Equal-area normalized gate matches legacy geometric area", "[tracker][association]")
{
  constexpr double equalRectangleAreaNis = 4.0 / 3.14159265358979323846;
  Tracker equalArea(1, 2, 5, 1, 0, 100, 2, -100, false, {}, true,
    equalRectangleAreaNis);
  Detection seed(20, 0, 8);
  equalArea.process(&seed, 1000);
  Detection outsideEqualArea(22, 0, 8);
  const auto tracks = equalArea.process(&outsideEqualArea, 2000);
  REQUIRE(tracks->get_n() == 2);
  CHECK(equalArea.frame_diagnostics().associations == 0);
}

TEST_CASE("Shared adaptive association fits one physical acceleration and strict boundaries", "[tracker][association]")
{
  using tracker_association::associationDecision;
  const auto gate=[](double delay,double doppler,double span=8.) {
    return associationDecision(delay,doppler,0,0,.5,true,.5,span,.125,true,9.210340371976184);
  };
  CHECK(gate(-1,4).accepted);
  CHECK(gate(-1,4).distance==0);
  CHECK_FALSE(gate(1.5,4).accepted); // Inside the box, no common acceleration.
  CHECK_FALSE(gate(-2,0).accepted); // Open outer boundary.
  CHECK_FALSE(gate(0,6).accepted);
  CHECK(gate(0,0,0).accepted); // Zero span is a singleton, not an empty interval.
  CHECK_FALSE(gate(1,0,0).accepted);
  CHECK_FALSE(gate(0,2,0).accepted);
  CHECK(associationDecision(2,0,0,0,1,false,1,0,0,true,4).accepted);
  CHECK_FALSE(associationDecision(std::nextafter(2.,3.),0,0,0,1,false,1,0,0,true,4).accepted);
  CHECK_FALSE(gate(std::numeric_limits<double>::infinity(),0).valid);
}

TEST_CASE("Shared spatial association checks all peaks and circular tolerance", "[tracker][association]")
{
  using tracker_association::spatialCompatible;
  const uint32_t multiple=0x80000000u|1u|(17u<<7u)|(33u<<14u);
  CHECK(spatialCompatible(multiple,5,12,4,0));
  CHECK(spatialCompatible(multiple,9,12,4,0));
  CHECK_FALSE(spatialCompatible(multiple,3,12,4,1));
  CHECK(spatialCompatible(12,1,12,4,1));
  CHECK_FALSE(spatialCompatible(12,2,12,4,1));
  CHECK(spatialCompatible(0,8,12,4,0));
  CHECK(spatialCompatible(1,8,12,4,UINT32_MAX));
  for(uint32_t beams:{4u,12u,24u})for(uint32_t a=1;a<=beams;++a)
    for(uint32_t b=1;b<=beams;++b)for(uint32_t tolerance:{0u,1u,2u}) {
      const uint32_t distance=a>b?a-b:b-a;
      const bool expected=std::min(distance,beams-distance)<=tolerance;
      CHECK(spatialCompatible(a,b,beams,4,tolerance)==expected);
      const uint32_t fine=0x80000000u|((a-1)*4+1);
      CHECK(spatialCompatible(fine,b,beams,4,tolerance)==expected);
    }
}

TEST_CASE("Beam-aware shared tracker retains accepted spatial history through coast and reset", "[tracker][association]")
{
  Tracker tracker(2,5,5,.5,0,100,1,5,false,{},false);
  tracker.configureSpatialAssociation(12,4,0);
  Detection seed(20,0,8);seed.set_spatial({0x80000000u|1u|(17u<<7u)});
  auto tracks=tracker.process(&seed,1000);const auto id=tracks->get_id(0);
  Detection secondary(20,0,8);secondary.set_spatial({5});
  tracks=tracker.process(&secondary,1500);
  REQUIRE(tracks->get_n()==1);CHECK(tracks->get_state(0)=="ACTIVE");
  CHECK(tracker.spatialForTrack(id)==5);
  Detection empty(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  tracks=tracker.process(&empty,2000);CHECK(tracker.spatialForTrack(id)==5);
  Detection incompatible(20,0,4);incompatible.set_spatial({1});
  tracks=tracker.process(&incompatible,2500);
  REQUIRE(tracks->get_n()==1);CHECK(tracks->get_nInactive(0)==2);
  CHECK(tracker.frame_diagnostics().pairSpatialRejected==1);
  CHECK(tracker.spatialForTrack(id)==5);
  Detection missing(20,0,8);
  CHECK_THROWS_AS(tracker.process(&missing,3000),std::invalid_argument);
  incompatible.set_spatial({13});CHECK_THROWS_AS(tracker.process(&incompatible,3000),std::invalid_argument);
  tracker.reset();CHECK(tracker.spatialForTrack(id)==0);
  CHECK_THROWS_AS(tracker.process(&missing,3000),std::invalid_argument);
  CHECK(tracker.process(&seed,3000)->get_n()==1);
}

TEST_CASE("Birth exclusivity runs after full association", "[tracker][birth]")
{
  BirthExclusivityConfig exclusive{true, 1.0, 1.0};
  Tracker tracker(1, 2, 5, 1, 0, 100, 2, -100, false, {}, false,
    9.210340371976184, exclusive);
  Detection seed(20, 0, 8);
  tracker.process(&seed, 1000);
  Detection next({20, 20.5}, {0, 0}, {8, 12});
  const auto tracks = tracker.process(&next, 2000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nAssociated(0) == 2);
  CHECK(tracker.frame_diagnostics().associations == 1);
  CHECK(tracker.frame_diagnostics().birthExclusiveRejected == 1);
}

TEST_CASE("Coasting predictions do not become measured associations", "[tracker]")
{
  Tracker tracker(2, 4, 10, .5, 10, 1000, .5, -100, true);
  Detection seed(10, 20, 12);
  auto tracks = tracker.process(&seed, 1000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nAssociated(0) == 1);
  CHECK(tracks->get_nUpdates(0) == 1);

  Detection none(std::vector<double>{}, std::vector<double>{},
    std::vector<double>{});
  tracks = tracker.process(&none, 1500);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nAssociated(0) == 1);
  CHECK(tracks->get_nUpdates(0) == 2);

  tracks = tracker.process(&none, 2000);
  REQUIRE(tracks->get_n() == 1);
  CHECK(tracks->get_nAssociated(0) == 1);
  CHECK(tracks->get_nUpdates(0) == 3);
}
