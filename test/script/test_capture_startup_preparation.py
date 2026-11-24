"""Keep cold qualification and consumer setup ahead of producer admission."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def startup_definition(source):
    """Extract the real scheduler, ignoring braces in comments and literals."""
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match[0]), source, flags=re.S)
    match = re.search(r"void\s+RadarBank::prepareStartup\s*\(\s*\)\s*\{", masked)
    if not match:
        raise AssertionError("Canonical startup scheduler is missing")
    depth = 1
    for end in range(match.end(), len(masked)):
        depth += (masked[end] == "{") - (masked[end] == "}")
        if not depth:
            return source[match.start():end + 1]
    raise AssertionError("Canonical startup scheduler is incomplete")


# Exercise the production scheduling body and real bounded executor without
# constructing DSP/GPU resources. Lane numerical/state parity remains covered
# by TestMultiIlluminatorRadar's startup preparation tests.
STARTUP_DRIVER = r'''
#include "process/utility/FixedExecutor.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
struct Probe {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned active = 0, peak = 0, arrived = 0, workers;
  int fail = -1;
  std::vector<unsigned> calls = std::vector<unsigned>(4);
  explicit Probe(unsigned count): workers(count) {}
  void prepare(unsigned lane) {
    std::unique_lock lock(mutex);
    ++calls.at(lane); ++active; ++arrived; peak = std::max(peak, active);
    changed.notify_all();
    const bool overlapped = changed.wait_for(lock, std::chrono::seconds(1),
      [&] { return arrived >= std::min(2u, workers); });
    --active;
    check(overlapped, "parallel startup did not overlap");
    if (int(lane) == fail) throw std::runtime_error("lane qualification failed");
  }
  void complete(unsigned count) {
    std::lock_guard lock(mutex);
    check(active == 0, "lane work survived startup return");
    for (auto callsForLane: calls)
      check(callsForLane == count, "lane preparation skipped or repeated");
  }
};
struct RadarBank {
  struct Context {
    Probe& probe; unsigned lane;
    void prepareStartup() { probe.prepare(lane); }
  };
  Probe probe;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::unique_ptr<blah2::utility::FixedExecutor> executor_;
  bool inputStarted_ = false, startupPrepared_ = false;
  explicit RadarBank(unsigned workers): probe(workers),
    executor_(std::make_unique<blah2::utility::FixedExecutor>(workers)) {
    for (unsigned lane = 0; lane < probe.calls.size(); ++lane)
      contexts_.push_back(std::make_unique<Context>(Context{probe, lane}));
  }
  void prepareStartup();
};
// PRODUCTION_STARTUP
int main(int argc, char**) try {
  if (argc > 1) {
    RadarBank serial(2);
    // Negative control: the retired serial schedule must fail this same probe.
    for (auto& context: serial.contexts_) context->prepareStartup();
    return 0;
  }
  for (unsigned workers: {1u, 2u}) {
    RadarBank bank(workers);
    const auto* owner = bank.executor_.get();
    bank.prepareStartup();
    check(bank.startupPrepared_, "completed startup was not admitted");
    check(bank.executor_.get() == owner, "startup replaced its bounded executor");
    bank.probe.complete(1);
    check(bank.probe.peak == workers, "startup ignored configured concurrency");
    bank.prepareStartup(); bank.probe.complete(1);
    bank.inputStarted_ = true;
    bool rejected = false;
    try { bank.prepareStartup(); } catch (const std::logic_error&) { rejected = true; }
    check(rejected, "startup accepted already-admitted input");
    bank.probe.complete(1);
  }
  RadarBank failed(2); failed.probe.fail = 0;
  bool rejected = false;
  try { failed.prepareStartup(); }
  catch (const std::runtime_error& error) {
    rejected = std::string(error.what()) == "lane qualification failed";
  }
  check(rejected && !failed.startupPrepared_, "failed qualification was admitted");
  failed.probe.complete(1); // Failure drains every lane before propagating.
  failed.probe.fail = -1;
  failed.prepareStartup(); failed.probe.complete(2);
  check(failed.startupPrepared_, "successful retry was not admitted");
  std::cout << "PASS bounded parallel startup ownership and failure draining\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n'; return 1;
}
'''


class StartupPreparation(unittest.TestCase):
    def test_native_float_capture_prepares_baseline_before_receiver_admission(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        prepare = source.index("buffer->enable_float_capture(nSamples, 2)")
        self.assertLess(prepare, source.index("t1 = std::thread("))
        self.assertLess(prepare, source.index("capture->process("))
        reserve = source.index("buffer->prepare_float_capture_reserve();")
        self.assertLess(prepare, reserve)
        self.assertLess(reserve, source.index("t1 = std::thread("))
        self.assertNotIn("enable_float_capture(nSamples, FloatCaptureBlockPool::warmAll)", source)
        pool = (ROOT / "src/data/FloatCaptureBlocks.h").read_text()
        acquire = pool.split("  Buffer acquire()", 1)[1].split("  void recycle", 1)[0]
        self.assertNotIn("prewarmed_", pool)
        self.assertLess(acquire.index("allocated_ == limit_"), acquire.index("Buffer result(blockSamples_)"))
        self.assertIn("std::min(warmBlocks, limit_)", pool)
        self.assertIn("--allocated_;", pool)
        self.assertIn("reserveTarget_ = 1;", pool)
        helper = pool.split("  void replenish()", 1)[1].split("public:", 1)[0]
        self.assertLess(helper.index("++allocated_; ++pending_;"), helper.index("lock.unlock();"))
        self.assertLess(helper.index("lock.unlock();"), helper.index("Buffer buffer ="))
        self.assertLess(helper.index("Buffer buffer ="), helper.index("lock.lock();"))
        self.assertIn("requested_ = false; // Retry only", helper)
        self.assertIn("allocated_ == limit_ && pending_", acquire)
        self.assertIn("reserveChanged_.wait(lock", acquire)

    def test_all_ingress_uses_one_bank_startup_before_threads(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        declaration = source.index("std::thread t1;")
        self.assertLess(source.index("multiRadar->prepareStartup();"), declaration)
        self.assertEqual(source.count("multiRadar->prepareStartup();"), 1)
        self.assertNotIn("acceleration.prepareStartup(", source)
        self.assertNotIn("resident->prepareStartup(", source)
        self.assertNotIn("cpuStages", source)
        bank = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn("residentFrontend->prepareStartup(", bank)
        self.assertIn("acceleration->prepareStartup(prepareFrame);", bank)
        self.assertEqual(source.count("capture->process("), 1)
        consumer = source.index("std::thread t2(")
        producer = source.index("t1 = std::thread(")
        self.assertLess(consumer, source.index("publishTerminalCaptureAccounting = [&]"))
        self.assertLess(source.index("publishTerminalCaptureAccounting = [&]"), producer)
        self.assertLess(producer, source.index("capture->process("))
        self.assertRegex(source, r"t2\.join\(\);\s*if \(t1\.joinable\(\)\) t1\.join\(\);")

    def test_synthetic_dsp_stops_before_products(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        warm = source.split("  void prepareStartup()", 1)[1].split("  void process_float_cpi", 1)[0]
        self.assertIn("startup::fillIq", warm)
        self.assertIn("if (!process_loaded_dsp())", warm)
        self.assertNotIn("queue_processed_cpi", warm)
        self.assertNotIn("stage_processed_cpi", warm)
        self.assertIn("stages->resetReference();", warm)
        self.assertIn("channel->clear();", warm)

    def test_terminal_evidence_follows_producer_join_and_counts_fifo_tail(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        consumer_end = source.split('capture->processing_failure_or_stop("Unknown processing failure");', 1)[1]
        self.assertIn("if (!state && t1.joinable())", consumer_end)
        self.assertLess(consumer_end.index("capture->request_stop();"), consumer_end.index("t1.join();"))
        self.assertLess(consumer_end.index("t1.join();"), consumer_end.index("publishTerminalCaptureAccounting();"))
        self.assertEqual(source.count("publishTerminalCaptureAccounting();"), 1)
        terminal = source.split("publishTerminalCaptureAccounting = [&]", 1)[1].split("// The consumer's queues", 1)[0]
        self.assertIn("captureBuffers[channel]->get_length()", terminal)
        self.assertRegex(terminal, r"consumerStopDiscarded\[channel\] = timing->capture_sample_sum\(\{consumerStopDiscarded\[channel\],\s*backlog\[channel\]\}\)")
        self.assertLess(terminal.index("consumerStopDiscarded[channel] ="), terminal.rindex("completeCycleEvidence();"))
        self.assertIn('fail_processing_evidence("terminal-capture-accounting-fault")', terminal)

    def test_parallel_startup_owns_and_drains_every_lane(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        driver = STARTUP_DRIVER.replace("// PRODUCTION_STARTUP", startup_definition(source))
        with tempfile.TemporaryDirectory(prefix="startup-preparation-") as temporary:
            binary = Path(temporary) / "startup"
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-pthread",
                       "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"),
                       "-x", "c++", "-", str(ROOT / "src/process/utility/FixedExecutor.cpp"),
                       "-o", str(binary)]
            subprocess.run(command, input=driver, text=True, check=True, timeout=30)
            completed = subprocess.run([str(binary)], text=True, capture_output=True, timeout=3)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("PASS bounded parallel startup", completed.stdout)
            serial = subprocess.run([str(binary), "serial-control"], text=True,
                                    capture_output=True, timeout=3)
            self.assertEqual(serial.returncode, 1, serial.stdout + serial.stderr)
            self.assertIn("parallel startup did not overlap", serial.stderr)

    def test_lane_profile_inherits_the_resolved_native_model(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        resolved = source.index('trackerAccelerationModel = trackerProfile == "balanced"')
        override = source.index('["acceleration_model"] >> trackerAccelerationModel', resolved)
        inherit = source.index('radarIntent.advanced.trackerAccelerationModel = trackerAccelerationModel;')
        self.assertLess(resolved, override)
        self.assertLess(override, inherit)
        self.assertLess(source.index('radarIntent.advanced = multiAdvanced;'), inherit)
        self.assertLess(inherit, source.index('auto multiConfig = radarIntent;'))

    def test_centroid_and_tracker_use_actual_caf_geometry(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn('p.cpiSeconds = shape.get_cpi();', source)
        self.assertIn('p.centroidDoppler = config.centroidDopplerRadius.value_or(config.centroidRadius);', source)
        pipeline = (ROOT / 'src/process/pipeline/RadarPipeline.h').read_text()
        self.assertTrue('centroid(config_.centroidDelay, config_.centroidDoppler, 1 / config_.cpiSeconds)' in pipeline)
        self.assertTrue('config_.cpiSeconds, config_.maximumAcceleration' in pipeline)
        native = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn('pipeline::resolveRadarPlan(', native)
        self.assertIn('residentConfig.detector = radarPlan.residentDetector();', native)

    def test_resident_startup_oracle_supports_dedicated_reference_roles(self):
        native = (ROOT / "src/blah2.cpp").read_text()
        self.assertNotIn('resident->prepareStartup(', native)
        bank = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        startup = bank.split('  void prepareStartup()', 1)[1].split('  void process_float_cpi', 1)[0]
        self.assertIn('ArrayReferenceSynthesizer::Metrics dedicated;', startup)
        self.assertIn('residentFrontend->checkReference(config.arrayReference ? reference :', startup)
        self.assertIn('*ownedChannels[config.referenceChannel]', startup)
        self.assertIn('stages->reference().get_metrics() : dedicated', startup)

    def test_rejected_cpi_cannot_reach_observer_or_publication(self):
        source = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        shared = source.split('  void process_cpi', 1)[1].split('  void process_loaded_cpi', 1)[0]
        self.assertRegex(shared, r'if \(!process_loaded_dsp\(\)\)\s*\{\s*reject\([^;]+;\s*resetAdaptiveState\(\);\s*return;')
        # A successful resident frontend returns before the host clutter path.
        # Both owned and borrowed ingress use this same rejection/publication
        # owner; fallback materialization occurs before host DSP only.
        self.assertIn('if (residentExecuted) return queue_processed_cpi(first);', shared)
        self.assertLess(shared.index('prepareHost();'), shared.index('if (!process_loaded_dsp())'))
        self.assertLess(shared.index('return;'), shared.rindex('queue_processed_cpi'))
        wrappers = source.split('  void process_loaded_cpi', 1)[1].split('  void prepareStartup', 1)[0]
        self.assertIn('process_cpi(first,', wrappers)
        self.assertIn('inputProcessing.path=input.isDouble()?"resident_borrowed_double":', wrappers)
        self.assertIn('"resident_borrowed_float";', wrappers)
        reset = source.split('  void resetAdaptiveState(', 1)[1].split('  void resetTracker()', 1)[0]
        self.assertIn('stages->resetReference();', reset)
        self.assertIn('products->reset(reason);', reset)
        self.assertNotIn('pendingFirstInputSample =', reset)

    def test_experimental_fm_filter_is_explicit_and_transport_independent(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn('bool experimentalFmReferenceQualityFilter = false;', source)
        self.assertIn('experimentalFmReferenceQualityFilter && !multiIlluminatorEnabled', source)
        self.assertIn('radarIntent.offlineReplay = state;', source)
        lane = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn('config.experimentalFmReferenceQualityFilter && definition.standard == "FM"', lane)
        admission = lane.split('if (config_.experimentalFmReferenceQualityFilter &&', 1)[1].split('const std::size_t available', 1)[0]
        self.assertNotIn('offlineReplay', admission)

    def test_shared_tracker_settings_require_active_detection_and_history(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        self.assertLess(source.index('tree["process"]["tracker"]["enable"] >> isTracker;'),
                        source.index('bool stationaryClutter=false;'))
        self.assertIn('stationaryClutter && !isTracker', source)
        contract = (ROOT / 'src/process/pipeline/CanonicalRadar.cpp').read_text()
        self.assertIn('p.recovery = config.trackerRecovery;', contract)
        pipeline = (ROOT / 'src/process/pipeline/RadarPipeline.h').read_text()
        self.assertTrue('result.configureRecovery(config_.recovery);' in pipeline)

    def test_resident_complex_cutout_is_rejected_before_recorder_construction(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        rejection = source.index('validateComplexCutoutRoute(complexCutoutConfig.enabled, state, accelerationMode)')
        construction = source.index('ComplexCutoutRecorder complexCutout(')
        self.assertLess(rejection, construction)
        admission = (ROOT / 'src/process/analysis/ExperimentalRouteAdmission.h').read_text()
        self.assertIn('enabled && (!replay || (acceleration != "cpu" && acceleration != "gpu"))', admission)
        self.assertIn('Experimental complex cutout requires replay with explicit CPU or portable GPU acceleration', admission)

    def test_direct_yaml_cannot_silently_enable_unimplemented_processing(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        self.assertIn('pipeline::Stages{isClutter, isDetection, isTracker}.validate();', source)
        self.assertNotIn('isTracker = false;', source)
        for admission in (
            'Multi-illuminator input phase correction is not available',
            'Multi-illuminator Doppler windows other than rectangular are not available',
            'Per-illuminator input_role_mode must be manual or automatic',
        ):
            self.assertIn(admission, source)
        self.assertIn('conditioning::resolveAtscReferenceMode(definition.standard,', source)
        self.assertIn('definition.atscReferenceMode, state)', source)
        radar = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        self.assertIn('conditioning::AtscPilotProjector', radar)
        self.assertIn('ambiguityReference = &projectedReference', radar)
        pipeline = (ROOT / 'src/process/pipeline/RadarPipeline.h').read_text()
        self.assertNotIn('ridge_.process', pipeline)
        self.assertIn('config.trackerDopplerRidge', radar)
        self.assertIn('config.offlineReplay, config.arrayReference)', radar)
        self.assertIn('validateDopplerRidgeAdmission(residentMode && !residentHostProducts,', source)
        self.assertIn('validateDopplerRidgeAdmission(residentProductRoute,', radar)

    def test_preparation_does_not_set_ready_or_skip_the_numerical_oracle(self):
        source = (ROOT / "src/process/ambiguity/Acceleration.cpp").read_text()
        warm = source.split("void Acceleration::prepareStartup(", 1)[1].split(
            "void Acceleration::retryClutterQualificationAfterStartup", 1)[0]
        self.assertIn("startupPending()", warm)
        self.assertIn("TotalQualificationFrames", warm)
        self.assertIn("processFrame(frame);", warm)
        self.assertNotRegex(warm, r"(?:checks_|combinedChecks_|clutterChecks_|status_)\s*=")

    def test_capture_discontinuity_keeps_resident_worker_and_drop_accounting(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        reset = source.split("if (extractedInputGeneration != inputGeneration || qualificationGap)", 1)[1]
        reset = reset.split("multiPipelineState.input_epoch_reset();", 1)[0]
        self.assertLess(reset.index("drainMultiPublications();"), reset.index("resetMultiRadar();"))
        self.assertNotIn("make_unique", reset)
        self.assertNotRegex(reset, r"(?:residentLogical|residentConsumed|residentConfig\.epoch|captureDroppedSamples)\s*(?:=|\+\+)")
        bank = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        adaptive = bank.split("  void resetAdaptiveState(", 1)[1].split("  void collectStorageMetrics", 1)[0]
        self.assertLess(adaptive.index("collectPending();"), adaptive.index("residentFrontend->resetInput();"))
        self.assertIn("products->reset(reason);", adaptive)
        self.assertNotIn("residentFrontend.reset()", adaptive)
        self.assertNotIn("make_unique", adaptive)

    def test_resident_qualification_keeps_strict_numerical_checks(self):
        source = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        self.assertIn("qualification_(Qualification::startupCount(Acceleration::TotalQualificationFrames,", source)
        self.assertIn("referenceOracleClose(value,expected)", source)
        proof = (ROOT / "src/process/ambiguity/GpuResidentProof.h").read_text()
        self.assertIn("std::abs(actual-expected)<=1e-12*std::max(1.0,std::abs(expected))", proof)
        self.assertIn("std::isfinite(actual.real())", proof)
        self.assertIn("std::isfinite(expected.real())", proof)
        for stage in ("reference", "filtered", "maps", "products"):
            self.assertIn(f"qualification_.checked(Qualification::{stage});", source)
        self.assertIn("qualification_.requireComplete();", source)
        reset = source.split(" void resetInput()", 1)[1].split(" const OutputViews&", 1)[0]
        self.assertIn("session_->resetInput();", reset)
        self.assertIn("clearBorrowed();qualification_.resetInput();", reset)
        self.assertNotIn("published()", reset)
        self.assertNotIn("status_", reset)

    def test_private_echo_calibration_never_observes_positive_or_public_maps(self):
        source = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        self.assertIn("if(privateStartup_&&!privatePositive_&&startupTarget_&&privateMature_)", source)
        self.assertIn("startupCalibration_.observeNegative", source)
        self.assertIn("startupCalibration_.amplitude(d.integrationCpis,startupGate_)", source)
        for start, end in ((" void resetInput()", " const OutputViews&"),
                           (" void stop()", " if(session_)session_->stop();")):
            body = source.split(start, 1)[1].split(end, 1)[0]
            self.assertIn("startupCalibration_.clear();startupTarget_.reset();startupGate_=0", body)
        calibration = (ROOT / "src/process/analysis/StartupIq.h").read_text()
        self.assertIn("updates_>=8", calibration)
        self.assertIn("std::abs(at-center)<=guard", calibration)
        self.assertIn("depth_!=depth", calibration)
        self.assertIn("value>4096", calibration)

    def test_live_resident_preparation_runs_every_oracle_without_publication(self):
        source = (ROOT / "src/blah2.cpp").read_text()
        canonical = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        warm = canonical.split("  void prepareStartup()", 1)[1].split("  void process_float_cpi", 1)[0]
        self.assertIn("residentFrontend->fillStartupIq(input, samples, frame);", warm)
        self.assertIn("residentFrontend->begin(ownedChannels, {uint64_t(frame)+1,", warm)
        self.assertIn("uint64_t(frame)+1, 1000 + uint64_t(frame) * samples * 1000 /", warm)
        self.assertNotIn("++residentLogical", warm)
        self.assertNotIn("++residentConsumed", warm)
        for call in ("process_loaded_dsp(true)", "residentFrontend->fuseOracle",
                     "residentFrontend->checkReference", "residentFrontend->checkFiltered",
                     "residentFrontend->checkMaps", "residentFrontend->checkProducts",
                     "residentFrontend->checkFrontendProducts()", "residentFrontend->commit()"):
            self.assertIn(call, warm)
        self.assertIn("coherentFusion.get(), oracleMap, stages->reference().get_metrics()", warm)
        self.assertIn("pipeline::ProductStages startupProducts", warm)
        self.assertNotIn("startupProducts.process", warm)
        self.assertIn("stages->resetReference();", warm)
        self.assertIn("reference.clear();", warm)
        self.assertIn("for (auto& channel : ownedChannels) channel->clear();", warm)
        for forbidden in ("capture->", "socket_", "complexCutout.", "liveTemporalShadow",
                          "timing->", "tracker->process", "coherentFusion->calibrate",
                          "residentFrontend->published", "saveMap", "manifoldRecorder.append",
                          "queue_processed_cpi", "stage_processed_cpi"):
            self.assertNotIn(forbidden, warm)
        self.assertNotRegex(warm, r"(?:residentLogical|residentConsumed|residentConfig\.epoch)\s*=")
        self.assertLess(source.index("multiRadar->prepareStartup("),
                        source.index('[Startup] DSP ready for capture'))

    def test_resident_preparation_fails_closed_and_resets_before_ready(self):
        source = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        warm = source.split(" void prepareStartup(", 1)[1].split(" const OutputViews&", 1)[0]
        self.assertIn("qualification_.prepareStartup([&](unsigned frame)", warm)
        self.assertIn("prepare(frame);", warm)
        self.assertIn("qualification_.maximumFrames()", warm)
        self.assertIn('" elapsed_ms="', warm)
        self.assertIn('" publish=0"<<std::endl', warm)
        self.assertIn("session_->published();", warm)  # transport settlement, no sink
        self.assertIn("qualification_.requireControls();", warm)
        self.assertIn("independentMapChecks_>=2*config().detector.integrationCpis", warm)
        self.assertLess(warm.index("qualification_.requireControls();"), warm.index("resetInput();resetCpu();"))
        self.assertIn("session_.reset();", warm)
        self.assertIn("session_=createProcess(productionConfig,options_);", warm)
        self.assertIn("session_->qualificationIdentity()==identity", warm)
        self.assertLess(warm.index("session_.reset();"), warm.index("session_=createProcess"))
        self.assertLess(warm.index("resetInput();resetCpu();"), warm.index('status_.state="ready"'))
        self.assertIn('stop();status_.state="failed";', warm)
        self.assertIn("throw;", warm)
        gate = (ROOT / "src/process/ambiguity/GpuResidentQualification.h").read_text()
        gate = gate.split(" void prepareStartup(", 1)[1]
        self.assertIn("if(completed_||verified_)", gate)
        self.assertIn("while(pending())", gate)
        self.assertIn("begin();prepare(observed_);requireComplete();settle();published();", gate)
        self.assertIn("uint64_t(observed_)>=uint64_t(required_)+depth_-1", gate)
        self.assertIn("reset();resetInput();", gate)
        self.assertNotRegex(gate, r"(?:completed_|required_)\s*=")

    def test_resident_temporal_qualification_uses_the_configured_canonical_products(self):
        direct = (ROOT / "src/blah2.cpp").read_text()
        lanes = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        coordinator = (ROOT / "src/process/ambiguity/GpuResidentCoordinator.h").read_text()
        for forbidden in ("cpuMapComparable", "cpuDetectorComparable", "cpuTrackerComparable"):
            self.assertNotIn(forbidden, coordinator)
        self.assertNotIn("startupProductConfig.integrationCpis = 1", direct)
        self.assertNotIn("productConfig.integrationCpis = 1", direct)
        self.assertNotIn("startupProducts.process", direct)
        self.assertNotIn("startupProducts.process", lanes)
        host = lanes.split("  RadarFrame complete_processed_cpi(", 1)[1].split(
            "  void queue_processed_cpi(", 1)[0]
        self.assertIn("products->process", host)
        self.assertIn("products->track", host)
        self.assertNotIn("cpuProducts", direct)
        self.assertNotIn("pipeline::ProductStages", direct)
        self.assertIn("pipeline::ProductStages startupProducts{RadarPlan(products->config())}", lanes)
        self.assertNotIn("oracleConfig.", lanes)
        self.assertIn('products->reset("startup_qualification_complete")', lanes)
        self.assertIn("cpuIntegration->availableCpis==resident.availableCpis", coordinator)
        composed = coordinator.split("MapOwner& independentMap,", 1)[1].split(
            "void checkProducts(const Publication& resident,const std::string& cpuMap,", 1)[0]
        self.assertIn("independentMapChecks_<2*p.integrationCpis", composed)
        self.assertIn("make_unique<detection::DetectorIntegration>", composed)
        self.assertIn("integration.availableCpis==resident.availableCpis", composed)
        self.assertLess(composed.index("independentMapOracle_->process"),
                        composed.index("compareOracleJson(independentMap->to_json_km"))
        self.assertLess(composed.index("compareOracleJson(independentMap->to_json_km"),
                        composed.index("productOracle_->process"))
        self.assertIn("independentMapChecks_==2*p.integrationCpis)independentMapOracle_.reset()", composed)
        self.assertGreaterEqual(coordinator.count("independentMapOracle_.reset()"), 5)
        self.assertGreaterEqual(coordinator.count("independentMapChecks_=0"), 5)
        self.assertIn("pipeline::RadarPlan(prototype.config())", composed)
        self.assertIn("productOracle_->process(map,position,true", composed)
        self.assertIn("&productOracle_->tracker,productOracle_->integration.get()", composed)
        self.assertNotIn("prepared_.detections", composed)
        self.assertNotIn("prepared_.flags", composed)
        self.assertGreaterEqual(coordinator.count("productOracle_.reset()"), 4)
        self.assertIn("qualification_.checkedProducts(temporal?resident.availableCpis:1)", coordinator)
        self.assertIn("fusion::BearingRefinement", coordinator)
        self.assertIn("metrics.covariance.data()", coordinator)
        self.assertIn("privatePositive_=frame>=d.integrationCpis", coordinator)
        self.assertIn("qualification_.control(privatePositive_,privateMature_", coordinator)
        self.assertIn("catch(const std::logic_error& failure)", coordinator)
        self.assertIn("throw std::logic_error(std::string(failure.what())+detail)", coordinator)
        self.assertIn("control_audit=unavailable", coordinator)
        self.assertIn('for(const auto& item:product["data"].GetArray())', coordinator)
        self.assertIn('if(state=="ACTIVE"||state=="COASTING")++confirmedTracks;', coordinator)
        self.assertIn("Qualification::confirmationFrame(d.integrationCpis,c.tracker.m)", coordinator)
        lane_startup = lanes.split("  void prepareStartup()", 1)[1].split("  void process_float_cpi", 1)[0]
        self.assertNotIn("++residentLogical", lane_startup)
        self.assertNotIn("++residentConsumed", lane_startup)
        self.assertIn("AUTO timing unavailable", lane_startup)

    def test_resident_history_reset_is_device_side_and_not_reconstruction(self):
        pipeline = (ROOT / "core/GpuResidentFrame.private.h").read_text()
        reset = pipeline.split(" void resetInput()", 1)[1].split(" bool prepareCaptured", 1)[0]
        for stage in ("referenceStage->reset()", "postprocessStage->appendInputReset",
                      "postprocessStage->completeInputReset", "tracker_->resetInput()"):
            self.assertIn(stage, reset)
        self.assertNotIn("make_unique", reset)
        post = (ROOT / "core/GpuPostprocess.private.h").read_text()
        reset = post.split(" void appendInputReset", 1)[1].split(" void append(", 1)[0]
        for history in ("integration_", "currentPower_", "angleHistory_", "tbdBanks_", "replayStatistics_"):
            self.assertIn(history, reset)
        self.assertIn("0x7ff80000u", reset)
        self.assertIn("79*4", reset)
        tracker = (ROOT / "core/GpuTracker.private.h").read_text()
        reset = tracker.split(" void resetInput()", 1)[1].split(" TrackerToken preparedToken", 1)[0]
        self.assertIn("clutterPersistence_->handle", reset)
        self.assertNotRegex(reset, r"(?:serial_|guidedBudgetExceeded_)\s*=")


if __name__ == "__main__":
    unittest.main()
