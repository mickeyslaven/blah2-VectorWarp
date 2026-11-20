"""Enforce canonical ownership; numerical/physical parity is tested separately."""
from pathlib import Path
import ast
import hashlib
import posixpath
import re
import subprocess
import unittest

# unittest discovers this imported TestCase too, including its negative controls.
# Keep the budget on the existing Linux/macOS architecture CI entry point.
from test_repository_budget import RepositoryBudgetControls
from test_repository_intelligence import ConfiguredInventoryControls, RepositoryIntelligenceControls

ROOT = Path(__file__).resolve().parents[2]
PRIMITIVES = {"clutter", "ambiguity", "conditioning", "fusion", "detection",
              "integration", "tracker", "meta", "utility", "pipeline"}
# These are existing public/publication contracts, not alternate processors.
PUBLICATION_COMPATIBILITY = {
    "src/process/analysis/MultiIlluminatorRadar.h",
    "src/process/analysis/MultiIlluminatorHealth.h",
    "src/process/analysis/MultiIlluminatorStatus.h",
    "src/process/analysis/MultiIlluminatorPipelineState.h",
}
PURPOSE_ROOTS = {
    ".github", "src", "core", "api", "html", "cmake", "packaging", "Formula",
    "services", "runtime", "config", "oprs", "contrib", "lib", "test", "bench",
    "vm-lab", "examples", "script", "tools", "docs", "schemas",
}
PURPOSE_FILES = {
    ".gitattributes", ".gitignore", "AGENTS.md", "CMakeLists.txt",
    "CMakePresets.json", "LICENSE", "README.md",
    "THIRD_PARTY_NOTICES.md",
}
STAGE = r"(?:CfarDetector1D|Centroid|Interpolate|Tracker|WienerHopf|Ambiguity)"
CONSTRUCTION = re.compile(
    rf"(?:new\s+{STAGE}\b|make_(?:unique|shared)<\s*{STAGE}\b|"
    rf"\b{STAGE}\s+[A-Za-z_]\w*\s*[({{])")
PRODUCT_CALL = re.compile(r"\b(?:detection::)?(?:detectProducts|trackProducts)\s*\(")
IDENTITY = re.compile(
    r"\b(?:Kraken|RspDuo|HackRF|Usrp|RTLSDR|Airspy|SoapySDR|"
    r"receiverType|deviceType|captureAdapter|pi4Host|__APPLE__|__linux__|_WIN32)\b",
    re.IGNORECASE)

# These are subordinate typed fusion owners, not native radar publishers. Their
# regression suites enforce provenance, ambiguity and no ADS-B inference input.
JS_FUSION_OWNERS = {
    'api/multi-illuminator-track-fusion.js',
    'api/cooperative-detection-tracker.js',
    'api/altitude-envelope-fusion.js',
}
OFFLINE_NUMERICAL_EXPORTS = {
    'fitManifold', 'fitDetectionManifold', 'evaluateManifoldCandidate',
    'evaluateDetectionManifoldCandidate', 'evaluateBearing',
    'evaluateReferenceDirection', 'evaluateReferenceSurvey',
}
JS_IMPORT = re.compile(r'''\brequire\s*\(\s*['"](\.[^'"]+)['"]\s*\)''')


def replay_process_writes(source):
    """Find plan writes, including aliases/get/setdefault, in replay admission."""
    function = next(node for node in ast.parse(source).body
                    if isinstance(node, ast.FunctionDef) and node.name == 'configuration')
    aliases = {'process': (), 'config': ('@config',)}

    def path(node):
        if isinstance(node, ast.Name):
            return aliases.get(node.id)
        if isinstance(node, ast.Subscript):
            base, key = path(node.value), node.slice
        elif (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
              and node.func.attr in ('get', 'setdefault') and node.args):
            base, key = path(node.func.value), node.args[0]
        else:
            return None
        if base is None or not isinstance(key, ast.Constant):
            return None
        result = base + (key.value,)
        return result[2:] if result[:2] == ('@config', 'process') else result

    nodes = list(ast.walk(function))
    for _ in range(len(nodes)):
        old = dict(aliases)
        for node in nodes:
            if isinstance(node, ast.Assign) and path(node.value) is not None:
                for target in node.targets:
                    if isinstance(target, ast.Name): aliases[target.id] = path(node.value)
        if aliases == old: break
    writes = []
    for node in nodes:
        targets = node.targets if isinstance(node, (ast.Assign, ast.Delete)) else (
            [node.target] if isinstance(node, (ast.AugAssign, ast.AnnAssign)) else [])
        for target in targets:
            if isinstance(target, ast.Subscript) and path(target) is not None:
                writes.append(path(target))
            elif isinstance(target, ast.Name) and target.id == 'process':
                if not (isinstance(node, ast.Assign) and ast.unparse(node.value) == "copy.deepcopy(profile['process'])"):
                    writes.append(('@replacement',))
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            base = path(node.func.value)
            if base is not None and node.func.attr in ('update', 'pop', 'popitem', 'clear', '__setitem__', '__delitem__'):
                writes.append(base + ('@' + node.func.attr,))
    return [value for value in writes if not value or value[0] != '@config']


def javascript_runtime_closure(sources, roots=('api/server.js',)):
    """Follow source dependencies, including renamed/wrapped offline modules."""
    pending, visited = list(roots), set()
    while pending:
        path = pending.pop()
        if path in visited or path not in sources:
            continue
        visited.add(path)
        for reference in JS_IMPORT.findall(code_only(sources[path])):
            target = posixpath.normpath(posixpath.join(posixpath.dirname(path), reference))
            resolved = next((name for name in (target, target + '.js', target + '/index.js')
                             if name in sources), None)
            if resolved:
                pending.append(resolved)
    return visited


def javascript_owner_violations(path, source):
    code = code_only(source)
    problems = []
    exports = '\n'.join(re.findall(r'module\.exports\s*=\s*\{([^}]+)\}', code, re.S))
    if OFFLINE_NUMERICAL_EXPORTS.intersection(re.findall(r'\b\w+\b', exports)):
        problems.append('offline numerical evaluation reachable from runtime')
    if path not in JS_FUSION_OWNERS:
        # Reading/forwarding a state is allowed; assigning a lifecycle literal
        # or generating a track identity is inference ownership, not transport.
        if re.search(r'''(?:\.\s*state\s*=(?!=)|\bstate\s*:)[^;\n]{0,180}['"](?:ACTIVE|TENTATIVE|CONFIRMED|COASTING|(?:FUSED|SUPPORT|COOPERATIVE|ENVELOPE|MAP_SUPPORT)_[A-Z_]+|PERSISTENT_AMBIGUOUS)['"]''', code):
            problems.append('radar lifecycle creation outside canonical fusion owners')
        if re.search(r'\bid\s*[:=][^;\n]{0,180}\b(?:nextId|nextTrackId)\s*\+\+', code):
            problems.append('radar identity creation outside canonical fusion owners')
    return problems


def code_only(source):
    # Keep strings: receiver-name dispatch in string comparisons is also forbidden.
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def files():
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT, check=True, capture_output=True).stdout.decode().split("\0")
    return sorted({p for p in result if p and (ROOT / p).is_file()})


def violations(path, source):
    code = code_only(source)
    problems = []
    parts = Path(path).parts
    canonical = (len(parts) > 2 and parts[:2] == ("src", "process")
                 and parts[2] in PRIMITIVES)
    # core/ is the explicitly reviewed private resident ABI/kernel boundary.
    backend = path.startswith("core/")
    if not canonical and not backend:
        if CONSTRUCTION.search(code) or PRODUCT_CALL.search(code):
            problems.append("numerical stage construction/orchestration outside canonical ownership")
        if re.search(r"\bProductConfig\b|\.\s*(?:detector|tracker)\.\w+\s*=(?!=)", code):
            problems.append("resolved product policy outside canonical plan ownership")
    if path.startswith("src/process/pipeline/"):
        if IDENTITY.search(code):
            problems.append("receiver/OS identity affects canonical processing")
        if re.search(r'#include\s+"(?:capture/|[^"]*(?:SubbandChannelizer|IlluminatorChannelizerBank))', code):
            problems.append("processing depends on an ingress implementation")
    if path.startswith(("src/capture/", "src/process/conditioning/")):
        if PRODUCT_CALL.search(code) or re.search(
                r'#include\s+"process/(?:detection|tracker)/', code):
            problems.append("ingress owns detection/tracking")
    if path.startswith("src/process/") and path not in PUBLICATION_COMPATIBILITY:
        if re.search(r"(?:Kraken|RspDuo|HackRF|Usrp|MixedProcess|MixedWorker|Pi4)", Path(path).name):
            problems.append("identity-named production processing path")
    return problems


def retention_problem(path):
    parts = Path(path).parts
    if any(p.lower() in {"frozen", "backup", "backups", "__pycache__", "node_modules",
                         "cmakefiles"} for p in parts):
        return "frozen/backup/generated tree"
    if path.endswith((".orig", ".bak", ".pyc", ".o", ".obj", ".a", ".so", "~")):
        return "backup/generated file"
    if Path(path).name in {"CMakeCache.txt", "compile_commands.json", ".DS_Store"}:
        return "generated build/host artifact"
    if path.startswith("src/process/mixed/") or re.fullmatch(
            r"config/config-(?:kraken|usrp|hackrf|pi4-rspduo)\.yml", path):
        return "retired duplicate processor/configuration"
    if parts[0] not in PURPOSE_ROOTS and path not in PURPOSE_FILES:
        return "no named repository responsibility"
    return None


class CanonicalProcessingArchitecture(unittest.TestCase):
    def test_runtime_javascript_has_no_offline_or_second_track_owner(self):
        sources = {path: (ROOT / path).read_text() for path in files()
                   if path.endswith('.js') and not path.endswith('.test.js')}
        reachable = javascript_runtime_closure(sources)
        self.assertIn('api/presentation-history.js', reachable)
        self.assertTrue(JS_FUSION_OWNERS.issubset(reachable))
        self.assertEqual([f'{path}: {problem}' for path in sorted(reachable)
                          for problem in javascript_owner_violations(path, sources[path])], [])

    def test_javascript_owner_guard_rejects_semantic_mutations(self):
        sources = {'api/server.js': "require('./adapter');",
                   'api/adapter.js': "require('../script/renamed');",
                   'script/renamed.js': 'module.exports = {evaluateBearing};'}
        self.assertEqual(javascript_runtime_closure(sources), set(sources))
        self.assertTrue(javascript_owner_violations('script/renamed.js', sources['script/renamed.js']))
        for mutation in ("return {...external, state: 'ACTIVE'};",
                         "track.state = 'TENTATIVE';",
                         "const id = 'anything' + this.nextId++;",
                         "return {id: `M${this.nextTrackId++}`};"):
            self.assertTrue(javascript_owner_violations('api/new-adapter.js', mutation), mutation)
        self.assertEqual(javascript_owner_violations('api/forward.js',
                         'return {id: native.id, state: native.state};'), [])
        self.assertEqual(javascript_owner_violations('api/forward.js',
                         "if (track.state === 'ACTIVE') send(track);"), [])

    def test_optional_wideband_failure_cannot_terminate_explicit_gpu_lanes(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        self.assertNotIn("multiQualificationFatal", main)
        self.assertNotIn("attemptWithAutoFallback", main)
        self.assertNotIn("cpiBoundedFrameDeadlineMs", main)
        self.assertIn("multiGpuOptions.offlineTransportTrace=state;", main)
        self.assertIn('multiGpuDevice=gpuDevice?gpuDevice:"auto";', main)
        self.assertRegex(main, r"wideband_gpu::createLeasedProcess\(\s*"
                              r"\*multiGpuConfig,multiGpuDevice,multiGpuOptions\)")
        # Reset, synchronous processing and split submit/finish share the same
        # optional-stage policy. Numeric and subprocess tests cover behavior.
        self.assertEqual(main.count("wideband_gpu::attemptOptionalWithFallback("), 5)
        self.assertIn("radarIntent.accelerationMode = accelerationMode;", main)
        self.assertIn("auto multiConfig = radarIntent;", main)

    def test_only_canonical_plans_resolve_product_math(self):
        main = code_only((ROOT / "src/blah2.cpp").read_text())
        pipeline = code_only((ROOT / "src/process/pipeline/RadarPipeline.h").read_text())
        bank = code_only((ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text())
        self.assertFalse((ROOT / "src/process/analysis/RadarAlgorithmContract.h").exists())
        self.assertIn("const ProductConfig products_;", pipeline)
        self.assertIn("const ProductConfig config_;", pipeline)
        self.assertIn("explicit ProductStages(const RadarPlan& plan)", pipeline)
        self.assertIn("const ProductConfig& products() const", pipeline)
        for adapter in (main, bank):
            self.assertIn("resolveRadarPlan(", adapter)
            self.assertIn(".residentDetector()", adapter)
            self.assertIn(".residentTracker(", adapter)
            self.assertNotRegex(adapter, r"(?:residentConfig|frontend)\.(?:detector|tracker)\.\w+\s*=")
        self.assertNotIn("ProductConfig", main)
        self.assertNotRegex(main, r"pfa\s*/")

    def test_canonical_fp64_readers_do_not_materialize_legacy_deques(self):
        # Compatibility belongs to IqData. Production readers must use its
        # nonallocating view; numerical/lifetime tests cover the actual result.
        readers = (
            "pipeline/CanonicalRadar.cpp", "pipeline/RawIqStages.cpp",
            "pipeline/RadarPipeline.h", "clutter/WienerHopf.cpp",
            "ambiguity/Ambiguity.cpp", "ambiguity/Acceleration.cpp",
            "ambiguity/GpuResidentCoordinator.h",
            "conditioning/ArrayReferenceSynthesizer.cpp",
            "conditioning/FmReferenceQualityFilter.h",
        )
        for reader in readers:
            source = code_only((ROOT / "src/process" / reader).read_text())
            self.assertNotRegex(source, r"(?:->|\.)\s*(?:view_data|get_data|resize_for_write)\s*\(", reader)

    def test_production_dsp_has_canonical_ownership(self):
        failures = []
        for path in files():
            if path.startswith(("src/", "core/")) and Path(path).suffix in {".h", ".cpp"}:
                failures.extend(f"{path}: {item}" for item in
                                violations(path, (ROOT / path).read_text()))
        self.assertEqual(failures, [])

    def test_ingress_and_publication_use_the_shared_processing_factories(self):
        main = (ROOT / "src/blah2.cpp").read_text()
        bank = (ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text()
        pipeline = (ROOT / "src/process/pipeline/RadarPipeline.h").read_text()
        for label, source in (("direct", main), ("signal bank", bank)):
            for call in ("pipeline::processFrontend(", "pipeline::fuseMaps(",
                         "pipeline::ProductStages", "pipeline::makeAmbiguity("):
                self.assertTrue(call in source, f"{label}: missing {call}")
        for forbidden in ("detection::detectProducts(", "detection::trackProducts(",
                          "acceleration.processClutter(", "mapFusion.process("):
            self.assertFalse(forbidden in main, f"publication owns {forbidden}")
        self.assertLess(pipeline.index("stages.clutter("), pipeline.index("stages.ambiguity("))
        self.assertLess(pipeline.index("integration->process("),
                        pipeline.index("detection::detectProducts("))
        self.assertLess(pipeline.index("detection::detectProducts("),
                        pipeline.index("detection::trackProducts("))
        self.assertTrue("config_.stages.detection" in pipeline)
        self.assertTrue("config_.stages.tracking" in pipeline)

    def test_source_compatibility_shims_cannot_grow_another_implementation(self):
        self.assertFalse((ROOT / "src/process/analysis/ProductionRawIq.h").exists())
        for name in ("MultiIlluminatorRadar",):
            path = ROOT / "src/process/analysis" / f"{name}.h"
            code = code_only(path.read_text())
            self.assertFalse(re.search(r"\b(?:class|struct|if|for|while)\b", code), str(path))
            self.assertFalse((path.with_suffix(".cpp")).exists())
        libraries = (ROOT / "cmake/ProcessingLibraries.cmake").read_text()
        canonical_links = libraries.split(
            "target_link_libraries(blah2MultiIlluminatorRadar PUBLIC", 1)[1].split(")", 1)[0]
        self.assertNotIn("blah2Channelizer", canonical_links)
        for implementation in ("CanonicalRadar.cpp", "RawIqStages.cpp"):
            self.assertEqual(libraries.count(f"/src/process/pipeline/{implementation}"), 1)
        for path in files():
            if path.endswith(".cmake") or Path(path).name == "CMakeLists.txt":
                source = (ROOT / path).read_text()
                self.assertFalse(re.search(r"analysis/(?:MultiIlluminatorRadar|ProductionRawIq)\.cpp",
                                           source), path)

    def test_canonical_data_cannot_carry_a_receiver_graph_selector(self):
        source = code_only((ROOT / "src/data/CanonicalSignal.h").read_text())
        self.assertIsNone(IDENTITY.search(source))
        self.assertFalse(re.search(r"\b(?:receiver|device|operatingSystem|channelizerKind)\s*[;={]", source))

    def test_canonical_ingestion_cannot_narrow_borrowed_fp64(self):
        storage = code_only((ROOT / "src/data/CanonicalSignal.h").read_text())
        self.assertIn("std::vector<IqSampleView>", storage)
        self.assertIn("std::variant<FloatChannels, DoubleChannels>", storage)
        self.assertIn("std::shared_ptr<const void> owner", storage)
        self.assertIn("std::complex<double>* destination", storage)
        source = code_only((ROOT / "src/process/pipeline/CanonicalRadar.cpp").read_text())
        ingest = source.split("std::vector<RadarFrame> consume(", 1)[1].split(
            "qualification_observer::Frame qualificationFrame", 1)[0]
        self.assertIn("directFloatBorrowed && !input.isDouble()", ingest)
        self.assertIn('input.isDouble() && config.accelerationMode != "cpu"', ingest)
        self.assertIn("input.copyChannelTo(", ingest)
        for text in (storage, ingest):
            self.assertNotRegex(text, r"(?:static_cast|reinterpret_cast)<[^>]*float")
            self.assertNotRegex(text, r"std::complex<float>\s*\(")

    def test_numerical_build_sources_have_canonical_owners(self):
        owners = ("cmake/ProcessingLibraries.cmake", "cmake/RadarProducts.cmake")
        registered = set()
        for owner in owners:
            registered.update(re.findall(r"\$\{PROJECT_ROOT\}/(src/[^\s)]+\.cpp)",
                                         (ROOT / owner).read_text()))
        # Capture-core/SDK doubles own the allocation ABI, not a second radar
        # algorithm. External DSP_ROOT/UPSTREAM_ROOT numerical oracles are
        # intentionally independent and cannot enter the production graph.
        registered.discard("src/data/IqData.cpp")
        for path in files():
            if path in owners or not (path.endswith(".cmake") or Path(path).name == "CMakeLists.txt"):
                continue
            source = (ROOT / path).read_text()
            for implementation in registered:
                self.assertNotRegex(source, r"(?<![\w}/])" + re.escape(implementation) + r"\b", path)
                self.assertNotIn("${PROJECT_ROOT}/" + implementation, source, path)

    def test_production_replay_has_one_session_and_no_test_owned_behavior(self):
        for path in files():
            if not (path.startswith("script/run-") and "replay" in path and path.endswith(".py")):
                continue
            source = (ROOT / path).read_text()
            self.assertIn("SESSION.ReplaySession(", source, path)
            for duplicate in ("subprocess.Popen(", "ThreadingHTTPServer(", "class SinkGroup",
                              "def stop_group(", "test/recording/", "HARNESS"):
                self.assertNotIn(duplicate, source, path)
        fixture = (ROOT / "test/recording/processor_replay_test.py").read_text()
        self.assertIn("VECTORWARP_REPLAY_SESSION", fixture)
        for duplicate in ("class SinkGroup", "class StatusServer", "def json_values("):
            self.assertNotIn(duplicate, fixture)

    def test_replay_preserves_processing_configuration(self):
        source = (ROOT / 'script/run-iq-protection-replay.py').read_text()
        self.assertEqual(replay_process_writes(source), [
            ('performance', 'acceleration'), ('bearing', 'manifold_recorder', 'enabled')])
        self.assertIn('if acceleration is not None:', source)
        self.assertIn('if manifold_evidence_export:', source)
        for runner in ('run-iq-protection-replay.py', 'run-isolated-iq-replay.py'):
            text = (ROOT / 'script' / runner).read_text()
            self.assertEqual(text.count('require_process_identity('), 3 if runner.startswith('run-iq') else 2)
            for option in ('--diagnostic-', '--stationary-clutter', '--protection-beams', '--protection-index'):
                self.assertNotIn(option, text)
        ui = (ROOT / 'html/js/config_ui.js').read_text()
        admission = ui.split('if (replayConfigurationComplete && recorded.profile)', 1)[1].split('// Side-profile', 1)[0]
        self.assertIn('activeConfig.process = JSON.parse(JSON.stringify(recordedProcess));', admission)
        self.assertNotIn('performance:', admission)

    def test_replay_guard_rejects_hidden_processing_mutations(self):
        for mutation in ("process['surveillance_fusion']['mode'] = 'noncoherent'",
                         "del process['legacy_field']", "process['performance'] = {}",
                         "process = {}", "process.get('detection', {}).update({'enable': False})",
                         "p = process.setdefault('tracker', {}); p.clear()",
                         "p = config['process']; p.pop('bearing')"):
            source = "def configuration():\n    process = copy.deepcopy(profile['process'])\n    " + mutation
            self.assertTrue(replay_process_writes(source), mutation)

    def test_explicit_experimental_math_is_transport_independent(self):
        main = (ROOT / 'src/blah2.cpp').read_text()
        target = (ROOT / 'src/process/analysis/ExperimentalRouteAdmission.h').read_text().split(
            'inline void validateTargetProtection', 1)[1].split('inline void validateComplexCutoutRoute', 1)[0]
        self.assertNotRegex(code_only(target), r'\breplay\b')
        projected = main.split('if(bearingReferenceAnchor=="fixed_site_projected")', 1)[1].split('auto process=', 1)[0]
        self.assertNotRegex(code_only(projected), r'\b(?:state|replayFormat)\b')
        self.assertIn('experimentalFmReferenceQualityFilter && !multiIlluminatorEnabled', main)
        lane = (ROOT / 'src/process/pipeline/CanonicalRadar.cpp').read_text().split(
            'if (config_.experimentalFmReferenceQualityFilter &&', 1)[1].split('const std::size_t available', 1)[0]
        self.assertNotIn('offlineReplay', lane)

    def test_frozen_clutter_is_authenticated_private_replay_control_only(self):
        for name in ('config/config.yml', 'api/config-rules.js', 'html/js/config_ui.js',
                     'src/process/ambiguity/GpuBackend.h'):
            self.assertNotRegex((ROOT / name).read_text(), r'clutter_checkpoint|fixed-weights|configureClutterReplay', name)
        main = (ROOT / 'src/blah2.cpp').read_text().split('qualification.has_child("observer")', 1)[0]
        for boundary in ('state&&!loop&&accelerationMode=="gpu"&&!multiIlluminatorEnabled&&isClutter',
                         'gpu_artifact_binding::active()', 'qualification.has_child("acquisition")'):
            self.assertIn(boundary, main)
        worker = (ROOT / 'src/process/ambiguity/GpuProcess.cpp').read_text()
        self.assertIn('if(qualification.clutter().active())', worker)
        backend = (ROOT / 'src/process/ambiguity/VulkanBackend.cpp').read_text()
        self.assertIn('std::unique_ptr<clutter_replay::State> replayClutter_;', backend)
        self.assertIn('if(!replay_||!replay_->fixed())', backend)

    def test_observation_cannot_select_product_ownership_or_install_experiments(self):
        main = (ROOT / 'src/blah2.cpp').read_text()
        self.assertIn('const bool residentHostProducts = residentMode && !coherentFusion;', main)
        self.assertIn('residentSnapshotEnabled&&(!state||!residentMode||residentHostProducts||multiIlluminatorEnabled||', main)
        cmake = (ROOT / 'CMakeLists.txt').read_text()
        runtime = cmake.split('foreach(runtime_target ', 1)[1].split('endforeach()', 1)[0]
        for tool in ('wienerHopfReplayInspect', 'directPathCoherentReplayInspect',
                     'residentReplayInspect', 'TemporalEndpointReplay'):
            self.assertNotIn(tool, runtime)
        for path in files():
            if not (path.endswith('.cmake') or Path(path).name == 'CMakeLists.txt'): continue
            for install in re.findall(r'install\s*\((.*?)\)', (ROOT / path).read_text(), re.S | re.I):
                self.assertNotRegex(install, r'wienerHopfReplayInspect|directPathCoherentReplayInspect|residentReplayInspect|TemporalEndpointReplay')

    def test_no_known_obsolete_duplicate_or_generated_categories(self):
        self.assertEqual([f"{p}: {retention_problem(p)}" for p in files()
                          if retention_problem(p)], [])
        # Distinct numerical/physical fixtures intentionally remain regression assets.
        self.assertTrue((ROOT / "test/gpu").is_dir())
        self.assertTrue((ROOT / "test/recording").is_dir())

    def test_regular_files_are_not_byte_for_byte_duplicates(self):
        # Symlink aliases (for example favicon.ico) are intentional and store no
        # second payload. Regular-file duplicates must instead share one source.
        seen = {}
        duplicates = []
        for relative in files():
            path = ROOT / relative
            if path.is_symlink():
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest in seen:
                duplicates.append(f"{relative} duplicates {seen[digest]}")
            else:
                seen[digest] = relative
        self.assertEqual(duplicates, [])

    def test_guards_reject_mutated_production_and_identity_dispatch(self):
        self.assertTrue(violations("src/capture/NewRoute.cpp", "Tracker tracker(1, 2);"))
        self.assertTrue(violations("src/blah2.cpp", "detection::detectProducts(map);"))
        self.assertTrue(violations("src/blah2.cpp", "pipeline::ProductConfig alternate;"))
        self.assertTrue(violations("src/capture/Adapter.cpp", "config.detector.pfa = .1;"))
        self.assertEqual(violations("src/process/analysis/Inspector.cpp",
                                   "if (config.detector.integrationCpis == 1) inspect();"), [])
        self.assertTrue(violations("src/process/analysis/NewProducts.cpp",
                                   "auto p = std::make_unique<CfarDetector1D>(x);"))
        self.assertTrue(violations("src/process/pipeline/New.cpp",
                                   'if (receiverType == "Kraken") runDifferentGraph();'))
        self.assertTrue(violations("src/process/pipeline/New.cpp",
                                   '#include "process/conditioning/SubbandChannelizer.h"'))
        for path in ("src/process/mixed/frozen/X.cpp", "test/backup/X.cpp",
                     "docs/report.md.bak", "src/generated.o"):
            self.assertIsNotNone(retention_problem(path), path)
        self.assertEqual(violations("src/process/pipeline/RadarPipeline.h",
                                    "Tracker tracker(config);"), [])


if __name__ == "__main__":
    unittest.main()
