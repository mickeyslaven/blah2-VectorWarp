#include "data/meta/StageExecution.h"
#include "data/meta/BackendMembership.h"
#include "process/analysis/NativeProductIdentity.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main() {
  try {
    rapidjson::Document document;
    auto& allocator = document.GetAllocator();
    const uint64_t first = (uint64_t(1) << 53) + 1;
    blah2::AccelerationStatus mixed;
    mixed.active = "mixed";
    mixed.members = {{"cpu-lane", "cpu", "", "selected", "", false, true, true, first, first + 100},
      {"gpu-lane", "vulkan-resident", "exact-device", "ready", "", true, false, true, first, first + 100}};
    mixed.members.push_back({"retry-lane", "cpu", "exact-device", "fallback", "CPU retry",
      true, true, true, first, first + 100, "vulkan-resident"});
    for (const bool known : {false, true}) {
      const auto members = blah2::backend_members_json(mixed, known, true, 0, allocator);
      require(members.Size() == 3 && std::string(members[1]["backend"].GetString()) == "vulkan-resident",
        "Mixed summary discarded exact backend membership");
      require(members[1]["execution"]["gpuExecuted"].GetBool() == known &&
        !members[0]["execution"]["gpuExecuted"].GetBool() &&
        members[0]["execution"]["cpuExecuted"].GetBool() == known,
        "Mixed execution inferred work from summary or leaked a cleared frame");
      require(members[1]["execution"]["frameBound"].GetBool() == known,
        "Cleared membership retained frame binding");
      require(std::string(members[2]["backend"].GetString()) == "cpu" &&
        std::string(members[2]["executedBackend"].GetString()) == (known ? "vulkan-resident" : "") &&
        members[2]["execution"]["gpuExecuted"].GetBool() == known &&
        members[2]["execution"]["cpuExecuted"].GetBool() == known,
        "CPU retry lost completed GPU identity or leaked a cleared frame");
      if (known) require(std::string(members[1]["execution"]["captureEpochExact"].GetString()) == "0" &&
        std::string(members[1]["execution"]["firstInputSampleExact"].GetString()) == std::to_string(first),
        "Mixed membership rounded or rejected exact source lineage");
    }
    for (const uint64_t epoch : {uint64_t(0), uint64_t(7), UINT64_MAX}) {
      const auto execution = blah2::stage_execution_json(true, true, false,
        true, epoch, first, first + 100, allocator);
      require(execution["available"].GetBool() && execution["frameBound"].GetBool(),
        "Valid source epoch did not bind completed stage work");
      require(execution["known"].GetBool() && execution["gpuExecuted"].GetBool() &&
        !execution["cpuExecuted"].GetBool(), "Stage execution flags differ");
      require(std::string(execution["captureEpochExact"].GetString()) == std::to_string(epoch) &&
        std::string(execution["firstInputSampleExact"].GetString()) == std::to_string(first) &&
        std::string(execution["endInputSampleExact"].GetString()) == std::to_string(first + 100),
        "Stage receipt rounded exact source lineage");
      blah2::analysis::NativeProductIdentity identity;
      identity.captureSamplePositionAvailable = true;
      identity.captureEpoch = epoch;
      identity.firstInputSample = first;
      identity.endInputSample = first + 98;
      identity.inputSamplesPerOutput = 2;
      const std::string resolvedPlan = "{\"schema\":\"vectorwarp-resolved-products-v1\",\"integrationCpis\":4}";
      identity.resolvedProductPlan = resolvedPlan;
      rapidjson::Document product;
      product.Parse(blah2::analysis::annotate_native_product("{}", identity).c_str());
      require(!product.HasParseError() && product["input_lineage"]["available"].GetBool(),
        "Valid source epoch did not bind numerical product lineage");
      require(std::string(product["input_lineage"]["captureEpochExact"].GetString()) ==
        std::to_string(epoch), "Product lineage changed the source epoch");
      require(std::string(product["input_lineage"]["endInputSampleExact"].GetString()) ==
        std::to_string(first + 100), "Product half-open interval differs");
      for (const char* backend : {"cpu", "vulkan", "vulkan-resident"}) {
        identity.productBackend = backend;
        product.Parse(blah2::analysis::annotate_native_product("{}", identity).c_str());
        require(std::string(product["resolved_product_plan"].GetString()) == resolvedPlan,
          "Ordinary products changed the canonical resolved plan at a backend boundary");
      }
      identity.productBackend="vulkan-resident";identity.productDevice="qualified-device";
      identity.temporalExecuted=true;identity.detectorExecuted=true;identity.trackerExecuted=true;
      identity.trackerOnHost=true;
      product.Parse(blah2::analysis::annotate_native_product("{}", identity).c_str());
      const auto& stages=product["product_stage_execution"];
      require(stages["temporal"]["execution"]["gpuExecuted"].GetBool()&&
        stages["detector"]["execution"]["gpuExecuted"].GetBool()&&
        !stages["tracker"]["execution"]["gpuExecuted"].GetBool()&&
        stages["tracker"]["execution"]["cpuExecuted"].GetBool()&&
        std::string(stages["tracker"]["backend"].GetString())=="cpu"&&
        std::string(stages["tracker"]["device"].GetString()).empty(),
        "Accelerated detector falsely labeled the canonical host tracker as GPU work");
      identity.productBackend="vulkan-temporal";identity.fusionOnHost=true;
      product.Parse(blah2::analysis::annotate_native_product("{}", identity).c_str());
      const auto& mapStages=product["product_stage_execution"];
      require(mapStages["fusion"]["execution"]["cpuExecuted"].GetBool()&&
        !mapStages["fusion"]["execution"]["gpuExecuted"].GetBool()&&
        mapStages["temporal"]["execution"]["gpuExecuted"].GetBool()&&
        mapStages["detector"]["execution"]["gpuExecuted"].GetBool()&&
        mapStages["tracker"]["execution"]["cpuExecuted"].GetBool(),
        "Map-only temporal acceleration changed host fusion/tracker provenance");
    }
    for (const bool positioned : {false, true}) {
      const auto unavailable = blah2::stage_execution_json(false, true, true,
        positioned, 0, first, first + 100, allocator);
      require(!unavailable["known"].GetBool() && !unavailable["frameBound"].GetBool() &&
        !unavailable["gpuExecuted"].GetBool() && !unavailable["cpuExecuted"].GetBool() &&
        !unavailable.HasMember("captureEpochExact"), "Unset stage work leaked a receipt");
    }
    for (const uint64_t end : {first, first - 1}) {
      const auto empty = blah2::sample_interval_json(true, 0, first, end, allocator);
      require(!empty["available"].GetBool() && !empty.HasMember("captureEpochExact"),
        "Empty or reversed source interval was accepted");
    }
    const auto hackrf = blah2::hackrf_cpi_sample_epoch(0, 0, 100, 1000);
    require(hackrf.present && hackrf.epoch == 0 && hackrf.firstSample == 0 &&
      hackrf.endSample == 100 && hackrf.reason.find("preserves a paired relative sample epoch") !=
        std::string::npos, "HackRF source epoch zero was treated as unavailable");
    require(!hackrf.estimateAvailable && !hackrf.deviceTimeAvailable,
      "Relative epoch was falsely promoted to an exact RF/UTC timestamp");
    require(blah2::hackrf_cpi_sample_epoch(0, 100, 100, 1000).reason ==
      "HackRF paired sample epoch is unavailable", "Empty HackRF interval was accepted");
    std::cout << "Stage execution and numerical lineage preserve zero/exact source epochs.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
