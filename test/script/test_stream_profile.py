"""Bounded diagnostic parsing and receive-path source contracts."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("stream_profile", ROOT / "tools/summarize_stream_profile.py")
PROFILE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROFILE)


class StreamProfileTests(unittest.TestCase):
    def test_quantiles_and_repeated_tails(self):
        values = PROFILE.metrics(list(range(1, 101)) + [890])
        self.assertEqual((values["p50"], values["p95"], values["p99"], values["max"]),
                         (51, 96, 100, 890))
        self.assertEqual(values["over500"], 1)

    def test_process_epoch_correlation_and_queue_slope(self):
        def cycle(pid, seq, start, queued, processing):
            return (f"journal[999]: STREAM_PROFILE_CYCLE pid={pid} sequence={seq} epoch=1 "
                    f"start_us={start} retired_us={start + processing * 1000} producer_queued={queued} "
                    f"pending_queued=10 processing_ms={processing}.0 completion_ms=500.0 idle_ms=50.0")
        summary = PROFILE.summarize("\n".join([
            cycle(10, 1, 1000000, 100, 450), cycle(10, 2, 1500000, 200, 890),
            cycle(11, 1, 3000000, 100, 450),
            "STREAM_PROFILE_WORK pid=20 role=wideband-worker identity=0 sequence=7 first=123 start_us=1550000 end_us=1600000 wall_ms=50.0",
            "STREAM_PROFILE_RECEIVE pid=10 mono_us=1600000 sampled=0 append_a_ms=150.0",
            "STREAM_PROFILE_RECEIVE_META pid=10 calls=500 cpu_stride=64 sampled=8",
        ]))
        self.assertEqual(len(summary["runs"]), 2)
        run = summary["runs"][0]
        self.assertEqual(run["pid"], "10")
        self.assertEqual(run["queue"]["endpoint_slope_samples_per_s"], 200)
        self.assertEqual(run["metrics_ms"]["completion_ms"]["n"], 1)
        self.assertEqual(run["largest_cycles"][0]["overlapping_work"][0]["sequence"], 7)
        self.assertEqual(run["largest_cycles"][0]["overlapping_receiver_tails"][0]["sampled"], 0)
        self.assertEqual(summary["receiver_sampling"][0]["cpu_stride"], 64)

    def test_ambiguous_duplicate_sequence_rejected(self):
        line = "STREAM_PROFILE_CYCLE epoch=1 sequence=1 start_us=1"
        with self.assertRaisesRegex(ValueError, "Repeated cycle sequence"):
            PROFILE.summarize(line + "\n" + line)

    def test_portable_parent_worker_and_clutter_correlation(self):
        lines = [
            "STREAM_PROFILE_WORK pid=10 role=portable-parent peer_pid=20 sequence=7 identity=6 start_us=1000 phase1_us=1100 phase2_us=1900 end_us=2000 wall_ms=1.0",
            "STREAM_PROFILE_WORK pid=20 role=portable-worker peer_pid=10 sequence=7 identity=6 start_us=1200 phase1_us=1700 phase2_us=1800 end_us=1850 wall_ms=0.65",
            "STREAM_PROFILE_CLUTTER pid=20 frame=4 start_us=1250 end_us=1600 wall_ms=0.35 host_covariance_fp64_ms=0.2 host_fence_wait_ms=0.05 thread_minor=3",
            # Another worker has the same sequence, and an incomplete parent
            # transaction must not silently correlate with it.
            "STREAM_PROFILE_WORK pid=21 role=portable-worker peer_pid=10 sequence=7 identity=6 start_us=1200 end_us=1800 wall_ms=0.6",
            "STREAM_PROFILE_WORK pid=10 role=portable-parent peer_pid=20 sequence=8 identity=2 start_us=2100 end_us=2200 wall_ms=0.1",
        ]
        result = PROFILE.summarize("\n".join(lines))
        tx = result["portable_transactions"]
        self.assertEqual((tx["matched"], tx["unmatched_parent"]), (1, 1))
        self.assertEqual(tx["records"][0]["worker"]["pid"], 20)
        self.assertEqual(tx["records"][0]["clutter"][0]["frame"], 4)
        self.assertEqual(tx["metrics_ms"]["worker_compute_ms"]["p99"], .5)
        self.assertAlmostEqual(sum(tx["records"][0]["metrics_ms"].values()), 1.0)
        self.assertEqual(result["clutter_profiles"][0]["activity_totals"]["thread_minor"], 3)
        self.assertEqual(result["clutter_profiles"][0]["metrics_ms"]["host_fence_wait_ms"]["max"], .05)
        with self.assertRaisesRegex(ValueError, "Repeated portable worker sequence"):
            PROFILE.summarize("\n".join(lines + [lines[1]]))

    def test_live_clutter_profile_is_bounded_and_does_not_enable_gpu_queries(self):
        source = (ROOT / "src/process/ambiguity/GpuClutterProfile.h").read_text()
        self.assertIn("liveLimit_=stream_profile::frameLimit()", source)
        self.assertIn("liveActive_=liveSize_<liveLimit_", source)
        self.assertIn("std::make_unique<LiveRecord[]>(liveLimit_)", source)
        self.assertLess(source.index("if (!limit_) return;"), source.index("vkCreateQueryPool"))
        finish = source.split("  void finish()", 1)[1]
        self.assertIn("if(frame_>limit_)", finish)
        self.assertNotIn("STREAM_PROFILE_CLUTTER", finish)  # teardown formatting only
        transport = (ROOT / "src/process/ambiguity/GpuWorkerTransport.private.h").read_text()
        self.assertIn("profileDrain?500:100", transport)
        self.assertLess(transport.index("drainDeadline"), transport.index("kill(process, SIGKILL)"))
        process = (ROOT / "src/process/ambiguity/GpuProcess.cpp").read_text()
        self.assertIn('stream_profile::Worker streamProfile("portable-worker",2)', process)
        self.assertIn("stream_profile::WorkScope profile(streamProfile,sequence,request.operation,getppid(),true)", process)
        backend = (ROOT / "src/process/ambiguity/VulkanBackend.cpp").read_text()
        complete = backend.split('  void complete(const char* phase)', 1)[1].split('  void submit(', 1)[0]
        self.assertEqual(complete.count("vkWaitForFences("), 1)
        self.assertIn("GpuClutterProfile::FenceWait", complete)

    def test_receive_records_buffered_outside_queue_locks(self):
        source = (ROOT / "src/capture/usrp/UsrpIngress.h").read_text()
        append = source.split("void append_usrp_block_committed", 1)[1]
        self.assertLess(append.index("secondLock.unlock();firstLock.unlock();"),
                        append.index("timing.profile.record("))
        self.assertNotIn("fprintf", append)
        profile = (ROOT / "src/data/meta/StreamProfile.h").read_text()
        self.assertIn('setting("VECTORWARP_STREAM_PROFILE_FRAMES",0,2048)', profile)
        record = profile.split("void record(ReceiveRecord", 1)[1].split("~Receiver", 1)[0]
        self.assertNotIn("fprintf", record)
        self.assertNotIn("new ", record)
        self.assertIn("s.size<s.limit", record)
        self.assertIn("std::array<char,4096> bytes_", profile)
        self.assertIn("frameLimit()*std::min(recordsPerFrame,2u)", profile)
        self.assertIn("std::fwrite(bytes_.data(),1,used_,stderr)", profile)
        self.assertNotIn("std::fputc", profile)

    def test_runtime_closure_includes_shared_core_and_transitive_layout(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        shared = cmake.split("add_executable(testCapturePoolAbi", 1)[1].split("include(cmake/JsonOutputTests.cmake)", 1)[0]
        self.assertIn("PRIVATE blah2CaptureCore", shared)
        self.assertNotIn("src/data/IqData.cpp", shared)
        closure = cmake.split("foreach(runtime_target", 1)[1].split("endforeach()", 1)[0]
        for target in ("blah2CaptureCore", "blah2GpuWorker", "blah2GpuVulkan", "blah2GpuWidebandVulkan",
                       "blah2GpuResidentVulkan", "blah2ReceiverUsrp", "blah2ReceiverHackrf", "blah2ReceiverRspduo"):
            self.assertIn(target, closure)
        self.assertIn("COMPONENT NativeRuntime", closure)
        cohort = (ROOT / "cmake/ReceiverModules.cmake").read_text().split("string(SHA256 BLAH2_RECEIVER_COHORT", 1)[0]
        self.assertIn("src/data/FloatCaptureBlocks.h", cohort)
        self.assertIn("src/data/IqSampleView.h", cohort)

    def test_native_spectrum_borrows_samples_without_cpi_materialization(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        publication = main.split("if (publishSpectrum) {", 1)[1].split("profileRecord.time[5]", 1)[0]
        self.assertIn("spectrumAnalyser->process(multiGpuStagingInput[referenceChannel]", publication)
        self.assertNotIn("resize_for_write", publication)
        self.assertNotIn("std::complex<double>(sample)", publication)
        self.assertIn("socket_iqdata->sendData(attachEventTime(", publication)
        self.assertIn("std::ceil(10.0 / tCpi)", main)
        source = (ROOT / "src/process/spectrum/SpectrumAnalyser.cpp").read_text()
        native = source.split("void SpectrumAnalyser::process(const std::vector", 1)[1].split("template<class Samples>", 1)[0]
        self.assertIn("processSamples(samples, output);", native)
        self.assertNotIn("resize", native)
        self.assertNotIn("new ", native)
        self.assertNotIn("assign", native)
        self.assertIn("std::complex<double>(data[offset + bin]) * blockPhase[block]", source)


if __name__ == "__main__":
    unittest.main()
