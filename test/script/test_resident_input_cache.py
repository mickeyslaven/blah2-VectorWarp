"""Bounded host tests for resident input placement and driver memory budgets.

Physical-device replay remains required for numeric and cadence qualification.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ResidentInputCache(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_driver_budget_reserves_headroom_and_charges_all_aliases(self):
        fixture = r'''
#include "process/ambiguity/GpuMemory.h"
#include <cassert>
using namespace blah2::gpu_memory;
constexpr uint64_t MiB=1ULL<<20, GiB=1ULL<<30;
int main(){
  // Without an actual driver budget, preserve conservative legacy admission.
  assert(heapAllowance({2*GiB,true})==512*MiB);
  assert(heapAllowance({256*MiB,true,256*MiB})==192*MiB);
  assert(heapAllowance({2*GiB,true,1536*MiB})==GiB);
  assert(heapAllowance({2*GiB,true,128*MiB})==0);
  assert(heapAllowance({UINT64_MAX,true,UINT64_MAX-1})==maximumDeviceBytes);
  Properties discrete{{{deviceLocal,0},{deviceLocal|hostVisible,0},{hostVisible,1}},
    {{2*GiB,true,1536*MiB},{8*GiB,false,6*GiB}}};
  auto policy=allocationPolicy(discrete,"2147483648");
  assert(policy.deviceBytes==GiB&&policy.bytes==2*GiB);
  AllocationBudget budget(policy,discrete);
  assert(budget.reserve(0,600*MiB)&&budget.reserve(1,400*MiB));
  assert(!budget.reserve(0,25*MiB));
  assert(budget.reserve(2,GiB)&&!budget.reserve(2,25*MiB));
  budget.release(0,600*MiB);
  assert(budget.reserve(0,600*MiB));
  // Unified memory has one shared allowance, not a second host allowance.
  Properties unified{{{deviceLocal|hostVisible|hostCoherent,0}},{{4*GiB,true,2*GiB}}};
  AllocationBudget uma(allocationPolicy(unified,nullptr),unified);
  assert(uma.limit()==GiB&&uma.reserve(0,GiB)&&!uma.reserve(0,1));
  // Low free VRAM refuses allocations, while a valid supervisor quota stays
  // tighter than both heap capacity and the driver-derived device allowance.
  Properties exhausted{{{deviceLocal,0}},{{2*GiB,true,128*MiB}}};
  bool rejected=false;
  try{allocationPolicy(exhausted,nullptr);}catch(const std::runtime_error&){rejected=true;}
  assert(rejected);
  AllocationBudget tight(allocationPolicy(discrete,"67108864"),discrete);
  assert(tight.reserve(0,64*MiB)&&!tight.reserve(2,1));
}
'''
        with tempfile.TemporaryDirectory(prefix="vectorwarp-input-cache-") as directory:
            executable = str(Path(directory) / "policy")
            result = subprocess.run([shutil.which("c++"), "-std=c++17", "-Wall", "-Werror",
                                     "-I", str(ROOT / "src"), "-x", "c++", "-", "-o", executable],
                                    input=fixture, text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([executable], check=True, capture_output=True, timeout=5)

    def test_upload_precedes_reference_and_diagnostics_copy_back_exact_channels(self):
        source = (ROOT / "core/GpuResidentFrame.private.h").read_text()
        self.assertIn("cacheInput&&!(canonical->properties&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)", source)
        self.assertIn("return deviceCanonical?*deviceCanonical:*canonical", source)
        self.assertIn("referenceStage->bind(radarInput())", source)
        reference = source.split('"resident reference recording"', 1)[1]
        self.assertLess(reference.index("canonical->handle,deviceCanonical->handle"),
                        reference.index("referenceStage->append(command)"))
        self.assertIn("radarInput().handle,replayRaw->handle", source)
        residual = source.split('"resident subtraction recording"', 1)[1].split(
            '"resident fused subtraction recording"', 1)[0]
        self.assertIn("uint64_t(config.surveillanceChannels[c])*n*16", residual)
        self.assertLess(residual.index("surveillancePack->append"),
                        residual.index("deviceCanonical->handle,canonical->handle"))
        self.assertLess(residual.index("deviceCanonical->handle,canonical->handle"),
                        residual.index("gpu_resident::hostRead(command)"))
        self.assertIn("for(bool cacheInput:{true,false})", source)
        self.assertIn("std::make_unique<Workspace>(*this,c,cacheInput)", source)
        self.assertIn("tracker_.reset();work_.reset()", source)

    def test_driver_budget_is_queried_only_when_supported(self):
        source = (ROOT / "src/process/ambiguity/VulkanResources.private.h").read_text()
        self.assertIn("VK_EXT_MEMORY_BUDGET_EXTENSION_NAME", source)
        self.assertIn('"vkGetPhysicalDeviceMemoryProperties2KHR"', source)
        self.assertIn("budget.heapUsage[i] < limit ? limit - budget.heapUsage[i] : 0", source)


if __name__ == "__main__":
    unittest.main()
