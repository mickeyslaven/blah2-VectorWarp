// Bounded serialization/admission probe consumed by api/frontend-plan.test.js.
#include "process/conditioning/FrontendPlan.h"
#include "process/analysis/NativeProductIdentity.h"
#include <iostream>
#include <type_traits>

int main() {
  using namespace blah2;
  static_assert(!std::is_copy_assignable_v<iq::FrontendPlan>);
  try {
    for (const unsigned variant : {0, 1, 2}) {
      const bool channelized = variant != 0;
      iq::SignalDefinition lane;
      lane.id = channelized ? "tv" : "primary";
      lane.frequencyHz = channelized ? 652000000 : 100000000;
      lane.outputRateHz = channelized ? 8000000 : 2000000;
      lane.occupiedBandwidthHz = channelized ? 7000000 : 2000000;
      lane.standard = channelized ? "DVB-T2" : "";
      lane.tapsPerPhase = variant == 2 ? 5 : 64;
      const uint64_t center = channelized ? 650000000 : 100000000;
      const uint32_t rate = channelized ? 24000000 : 2000000;
      const iq::FrontendPlan plan(center, rate, 2, lane, channelized, {0}, {1});
      iq::CoherentSamples samples;
      samples.channels.resize(2);
      samples.inputSamplesPerOutput = channelized ? 3 : 1;
      samples.groupDelayInputSamples = channelized ? (variant == 2 ? 8 : 96) : 0;
      plan.validate(samples);
      const auto refuses = [&](auto change) {
        auto wrong = samples; change(wrong);
        try { plan.validate(wrong); } catch (const std::invalid_argument&) { return; }
        throw std::runtime_error("frontend accepted mismatched lineage");
      };
      refuses([](auto& s) { s.inputSamplesPerOutput++; });
      refuses([](auto& s) { s.groupDelayInputSamples++; });
      refuses([](auto& s) { s.channels.pop_back(); });
      if (channelized) refuses([](auto& s) { s.hasFirstInputSample = true; s.firstInputSample = 1; });
      analysis::NativeProductIdentity identity;
      identity.resolvedFrontendPlan = plan.identity();
      identity.firstInputSample = 9007199254740993ULL;
      identity.endInputSample = identity.firstInputSample + 10;
      identity.captureSamplePositionAvailable = true;
      std::cout << analysis::annotate_native_product("{}", identity) << '\n';
    }
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
