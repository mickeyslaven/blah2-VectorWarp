"""Behavioral negative controls for the CI repository maintainability ratchet."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("repository_budget", ROOT / "script/check_repository_budget.py")
BUDGET = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUDGET)


class RepositoryBudgetControls(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.put("src/process/pipeline/Canonical.cpp", "int canonical = 1;\n")
        self.put("api/package.json", '{"dependencies":{"express":"1"}}\n')
        self.put("config/config.yml", "capture:\n  source: raw\n")
        self.put("test/test_behavior.py", "# useful regression evidence\n")
        metrics, failures = BUDGET.measure(self.root, self.paths(), {})
        self.assertEqual(failures, [])
        self.document = {"schema_version": 1, "snapshot": {"metrics": metrics, "exclusions": {}},
                         "adjustments": []}
        self.seal = BUDGET.digest(self.document["snapshot"])

    def put(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def paths(self):
        return [path.relative_to(self.root).as_posix() for path in self.root.rglob("*") if path.is_file()]

    def failures(self):
        return BUDGET.check(self.root, self.document, self.paths(), self.seal)[1]

    def approve(self, key, after):
        before = BUDGET.limits(self.document, self.seal).get(key, 0)
        self.document["adjustments"].append({
            "metric": key, "from": before, "to": after,
            "owner": "VectorWarp maintainers", "purpose": "Cover a required supported capability",
            "rejected_reuse": "Existing owner cannot express this contract",
            "cleanup_plan": "Consolidate after the compatibility consumer retires"})

    def test_current_repository(self):
        _, failures = BUDGET.check()
        self.assertEqual(failures, [], "\n".join(failures))

    def test_reviewed_adjustment_history_remains_sealed(self):
        # Extend review sealing without rebasing either original snapshot.
        # Later exact adjustments may append; no reviewed ceiling may vanish.
        adjustments = json.loads((ROOT / BUDGET.BASELINE).read_text())["adjustments"]
        self.assertGreaterEqual(len(adjustments), 72)
        self.assertEqual(BUDGET.digest(adjustments[:72]),
                         "da95057fd622a56290a54246af1e79e6471424dc7563ed6d22ea2488cb208ff2")
        self.assertTrue(all(after < before for before, after in adjustments[71]["changes"].values()))
        # Exact canonical-contract exceptions retain the original ceilings and
        # review history; the final entry seals actual source consolidation.
        self.assertGreaterEqual(len(adjustments), 92)
        self.assertEqual(BUDGET.digest(adjustments[:92]),
                         "cda5dc6992a5cb33429b8cc018df0b5aef82d92fe8bb23374aef8b3ef3f7dd0c")
        self.assertTrue(all(after < before for before, after in adjustments[91]["changes"].values()))
        # Closing the hosted offline-suite routing gap is an exact reviewed
        # exception, not a rebase or qualification of physical hardware.
        self.assertGreaterEqual(len(adjustments), 93)
        self.assertEqual(BUDGET.digest(adjustments[:93]),
                         "9ff628824e456ede92d98b87fd640e54cde53b48d3a1c4830d860a67fa300800")
        # Private bounded checkpoint proof and canonical live/replay admission
        # retain exact per-owner exception debt; actual reductions are mandatory.
        self.assertGreaterEqual(len(adjustments), 105)
        self.assertEqual(BUDGET.digest(adjustments[:105]),
                         "63d990c4fa51e35ead6822d5fbecb660f3e942e568ac0c6e13a9f336e31090e4")
        self.assertEqual(sum(len(row["changes"]) for row in adjustments[93:104]), 51)
        self.assertTrue(all(after > before for row in adjustments[93:104]
                            for before, after in row["changes"].values()))
        self.assertEqual(len(adjustments[104]["changes"]), 27)
        self.assertTrue(all(after < before for before, after in adjustments[104]["changes"].values()))
        # Warning-clean formatting adds three lexical tokens, not source lines;
        # its six-character reduction is sealed through every direct consumer.
        self.assertGreaterEqual(len(adjustments), 106)
        self.assertEqual(BUDGET.digest(adjustments[:106]),
                         "f0dc6c7587e8995f45f3fb3864f3414755ead8b10a820b1378eba704952358a2")
        # Explicit control-flow braces/newlines retain the same private contract.
        self.assertGreaterEqual(len(adjustments), 107)
        self.assertEqual(BUDGET.digest(adjustments[:107]),
                         "0bf382b9a05a1437234c9b792d21d60722327d8cabf84af4a373b888407385a2")
        # Optional checkpoint completion never controls the unconditional return.
        self.assertGreaterEqual(len(adjustments), 108)
        self.assertEqual(BUDGET.digest(adjustments[:108]),
                         "839de77b68da77e07382b1a5b66609da1bd96d1a6d699d83ee0c850aa67cedac")
        # Final compiler-warning formatting retains all factor/mutation behavior.
        self.assertGreaterEqual(len(adjustments), 109)
        self.assertEqual(BUDGET.digest(adjustments[:109]),
                         "1926df2823034a6d8fc48cafaffc7241c184999437244d215dbd0c5d11c8ab9b")
        # Shared tracker policy and the prebuild gate retain exact owner costs;
        # removed private tracker copies permanently lower their ceilings.
        self.assertGreaterEqual(len(adjustments), 120)
        self.assertEqual(BUDGET.digest(adjustments[:120]),
                         "2231c2847f098aa53dc0205f00fabb61755785f44b48fab9388d033a74c720cd")
        self.assertEqual(sum(len(row["changes"]) for row in adjustments[109:119]), 50)
        self.assertTrue(all(after > before for row in adjustments[109:119]
                            for before, after in row["changes"].values()))
        self.assertEqual(len(adjustments[119]["changes"]), 7)
        self.assertTrue(all(after < before for before, after in adjustments[119]["changes"].values()))
        # Lifecycle repair banks occupancy and advances only accepted frames;
        # its changed owner/contract costs include six mandatory reductions.
        self.assertGreaterEqual(len(adjustments), 121)
        self.assertEqual(BUDGET.digest(adjustments[:121]),
                         "a41b59b23c4bc671784f4c550e14d2b8ccd5736166d87c5d7c67aa606fba1580")
        self.assertEqual(sum(after > before for before, after in adjustments[120]["changes"].values()), 17)
        self.assertEqual(sum(after < before for before, after in adjustments[120]["changes"].values()), 6)
        # CI consumes the canonical Node pin and preserves macOS shell options.
        self.assertGreaterEqual(len(adjustments), 122)
        self.assertEqual(BUDGET.digest(adjustments[:122]),
                         "a8a8f177773cb5f18acbdd3aa4e875e1749e21efcae0f0c6b0451ab6893bc132")
        self.assertEqual(len(adjustments[121]["changes"]), 14)
        self.assertTrue(all(after > before for before, after in adjustments[121]["changes"].values()))
        # Platform declarations and peer-close handling retain canonical owners.
        self.assertGreaterEqual(len(adjustments), 123)
        self.assertEqual(BUDGET.digest(adjustments[:123]),
                         "98d7f7c5eaa7e74521720ca8a4b3b640f8245c15be3e5a0d64dd1bcdd7a207f2")
        self.assertEqual(len(adjustments[122]["changes"]), 23)
        self.assertTrue(all(after > before for before, after in adjustments[122]["changes"].values()))
        # Shared tracker rounding and platform fixes retain both exact reductions.
        self.assertGreaterEqual(len(adjustments), 124)
        self.assertEqual(BUDGET.digest(adjustments[:124]),
                         "d95931291a081aa777b1cbdfed947d82ff973454c3b2dd43330565dfe0edc55a")
        self.assertEqual(sum(after > before for before, after in adjustments[123]["changes"].values()), 22)
        self.assertEqual(sum(after < before for before, after in adjustments[123]["changes"].values()), 2)
        # Optional lane overrides forward to the unchanged transaction owner.
        self.assertGreaterEqual(len(adjustments), 125)
        self.assertEqual(BUDGET.digest(adjustments[:125]),
                         "9eeac02c36a946716f9e4f3a501f347375ea58d962cdeb4859b521b8a6993ab0")
        self.assertEqual(len(adjustments[124]["changes"]), 10)
        self.assertTrue(all(after > before for before, after in adjustments[124]["changes"].values()))
        # Preserve the earlier candidate ratchets, then seal each canonical
        # owner review and the removed direct-runtime/alias contract edges.
        self.assertGreaterEqual(len(adjustments), 166)
        self.assertEqual(BUDGET.digest(adjustments[:131]),
                         "10c6764d1c63fe656f47afd510aa200aee1538db121260710edda17cf32d1295")
        self.assertEqual(BUDGET.digest(adjustments[:166]),
                         "ef3042ddac061739fb401a1dc57aed23b8f4243f17c68c0a41798b3369c2b05b")
        self.assertEqual(sum(len(row["changes"]) for row in adjustments[131:166]), 224)
        self.assertEqual(sum(after < before for row in adjustments[131:166]
                             for before, after in row["changes"].values()), 22)

    def test_checkout_line_endings_do_not_change_metrics(self):
        before = BUDGET.measure(self.root, self.paths(), {})
        for path in self.paths():
            target = self.root / path
            target.write_bytes(target.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(BUDGET.measure(self.root, self.paths(), {}), before)

    def test_source_cannot_hide_behind_an_excluded_symlink_target(self):
        self.put("docs/hidden.js", "const hidden = 1;\n")
        try:
            (self.root / "api/forward.js").symlink_to("../docs/hidden.js")
        except OSError:
            self.skipTest("host does not permit test symlinks")
        self.assertTrue(any("bypass budgets through a symlink" in failure for failure in self.failures()))

    def test_actual_tree_includes_untracked_and_force_added_artifacts(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.put(".gitignore", "build*/\n")
        self.put("build-local/ignored.o", "local build")
        self.put("build-local/committed.o", "accidentally committed build")
        subprocess.run(["git", "add", "-f", "build-local/committed.o"], cwd=self.root, check=True)
        paths = BUDGET.source_paths(self.root)
        self.assertIn("src/process/pipeline/Canonical.cpp", paths)
        self.assertIn("build-local/committed.o", paths)
        self.assertNotIn("build-local/ignored.o", paths)
        (self.root / "build-local/committed.o").unlink()
        self.assertNotIn("build-local/committed.o", BUDGET.source_paths(self.root))

    def test_new_production_file_and_line_growth_fail(self):
        self.put("api/extra.js", "const x = 1;\n")
        failures = "\n".join(self.failures())
        self.assertIn("files:api", failures)
        self.assertIn("lines:api", failures)

    def test_existing_file_growth_fails_without_a_new_file(self):
        self.put("src/process/pipeline/Canonical.cpp", "int canonical = 1;\n\n")
        self.assertTrue(any("lines:src" in failure for failure in self.failures()))

    def test_tests_and_evidence_do_not_fund_production_growth(self):
        self.put("test/test_behavior.py", "# regression\n" * 4000)
        self.put("docs/evidence.json", json.dumps({"physical_receipt": list(range(3000))}))
        self.assertEqual(self.failures(), [])
        (self.root / "test/test_behavior.py").unlink()
        self.put("src/process/pipeline/New.cpp", "int replacement = 2;\n")
        self.assertTrue(any("files:src" in failure for failure in self.failures()))

    def test_new_oversized_file_fails_even_with_aggregate_headroom(self):
        self.put("src/process/pipeline/Canonical.cpp", "x\n" * (BUDGET.MAX_LINES + 1))
        self.approve("lines:src", BUDGET.MAX_LINES + 1)
        self.assertTrue(any("oversized_lines:" in failure for failure in self.failures()))
        self.put("src/process/pipeline/Canonical.cpp", "x" * (BUDGET.MAX_BYTES + 1))
        self.assertTrue(any("oversized_bytes:" in failure for failure in self.failures()))

    def test_dependency_replacement_cannot_hide_behind_equal_counts(self):
        self.put("api/package.json", '{"dependencies":{"new-package":"1"}}\n')
        self.assertTrue(any("dependency:api/package.json:dependencies:new-package" in failure
                            for failure in self.failures()))
        self.put("api/package.json", '{"dependencies":{"express":"2"}}\n')
        self.assertEqual(self.failures(), [])

    def test_manifest_and_vcpkg_features_are_inventoried(self):
        self.put("test/package.json", '{"dependencies":{"test-helper":"1"}}')
        self.assertTrue(any("manifest:test/package.json" in failure for failure in self.failures()))
        keys = BUDGET.dependency_keys("lib/vcpkg.json", json.dumps({"dependencies": [
            {"name": "asio", "features": ["tls"]}], "features": {"gpu": {
                "dependencies": ["vulkan"]}}}))
        self.assertIn("dependencies:asio:feature:tls", keys)
        self.assertIn("feature:gpu:vulkan", keys)
        with self.assertRaisesRegex(ValueError, "reviewed dependency parser"):
            BUDGET.dependency_keys("test/requirements-extra.txt", b"package==1")

    def test_duplicate_and_new_configuration_fail(self):
        self.put("config/copied.yml", "# different comment\ncapture:\n  source: raw\n")
        failures = "\n".join(self.failures())
        self.assertIn("duplicates configuration", failures)
        self.assertIn("configuration:config/copied.yml", failures)

    def test_parallel_directory_is_not_hidden_by_file_count(self):
        (self.root / "src/process/pipeline/Canonical.cpp").unlink()
        self.put("src/process/alternate/Canonical.cpp", "int canonical = 1;\n")
        self.assertTrue(any("processing_directory:src/process/alternate" in failure
                            for failure in self.failures()))

    def test_generated_vendor_and_build_files_fail_even_under_tests(self):
        for path in ("test/vendor/copy.py", "test/generated/output.cpp", "build-ci/output.json",
                     "src/library.dll", "test/dist/bundle.js", "test/CMakeCache.txt"):
            with self.subTest(path=path):
                self.assertIsNotNone(BUDGET.artifact_problem(path))
        self.assertIsNone(BUDGET.artifact_problem("api/third_party/library/PROVENANCE.md"))

    def test_exclusions_are_exact_and_vendor_payload_is_pinned(self):
        vendor = "html/lib/thirdparty.min.js"
        self.put(vendor, "x" * (BUDGET.MAX_BYTES + 1))
        self.document["snapshot"]["exclusions"][vendor] = {
            "kind": "third_party_minified", "sha256": hashlib.sha256(
                (self.root / vendor).read_bytes()).hexdigest()}
        self.seal = BUDGET.digest(self.document["snapshot"])
        self.assertEqual(self.failures(), [])
        self.put(vendor, "first party pretending to be vendor")
        self.assertTrue(any("exclusion digest changed" in failure for failure in self.failures()))
        self.put("test/package-lock.json", '{"lockfileVersion":3}')
        self.assertTrue(any("unreviewed generated lockfile" in failure for failure in self.failures()))

    def test_silent_snapshot_refresh_and_metadata_omission_fail(self):
        changed = copy.deepcopy(self.document)
        changed["snapshot"]["metrics"]["lines:src"] += 1
        with self.assertRaisesRegex(ValueError, "instead of rebasing"):
            BUDGET.limits(changed, self.seal)
        self.approve("lines:src", 2)
        for field in BUDGET.REVIEW_FIELDS:
            changed = copy.deepcopy(self.document)
            del changed["adjustments"][0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                BUDGET.limits(changed, self.seal)

    def test_adjustment_is_exact_chained_and_cannot_waive_other_metrics(self):
        self.put("src/process/pipeline/Canonical.cpp", "int canonical = 1;\n\n")
        self.approve("lines:src", 2)
        self.assertEqual(self.failures(), [])
        self.put("src/process/pipeline/Extra.cpp", "int extra;\n")
        self.assertTrue(any("files:src" in failure for failure in self.failures()))
        self.document["adjustments"][0]["from"] = 99
        with self.assertRaisesRegex(ValueError, "non-chained"):
            self.failures()
        self.document["adjustments"][0]["metric"] = "lines:*"
        with self.assertRaisesRegex(ValueError, "exact supported metric"):
            self.failures()

    def context_document(self):
        policy = {'preferred_owner_characters': 128000, 'preferred_packet_characters': 192000,
                  'owners': [{'owner': name, 'patterns': [prefix]} for name, prefix in
                             [('canonical', 'src/*'), ('api', 'api/*'), ('configuration', 'config/*')]]}
        metrics, _ = BUDGET.measure_context(self.root, self.paths(), {}, policy)
        document = copy.deepcopy(self.document)
        document['schema_version'] = 2
        document['context_snapshot'] = {'policy': policy, 'metrics': metrics,
            'sealed_adjustments': {'count': len(document['adjustments']),
                                   'sha256': BUDGET.digest(document['adjustments'])}}
        return document, BUDGET.digest(document['context_snapshot'])

    def context_failures(self, document, seal):
        return BUDGET.check(self.root, document, self.paths(), self.seal, seal)[1]

    def test_context_counts_characters_and_lexical_tokens_not_only_lines(self):
        document, seal = self.context_document()
        self.put('src/process/pipeline/Canonical.cpp', 'int substantially_longer_identifier = 1;\n')
        failures = '\n'.join(self.context_failures(document, seal))
        self.assertIn('context_characters:canonical', failures)
        self.assertIn('context_characters:production', failures)
        self.put('src/process/pipeline/Canonical.cpp', 'int a=1;int b=2;int c=3;\n')
        self.assertTrue(any('context_tokens:canonical' in item
                            for item in self.context_failures(document, seal)))

    def test_context_contract_replacement_cannot_hide_behind_equal_counts(self):
        self.put('api/one.js', "module.exports=1;\n")
        self.put('api/two.js', "module.exports=2;\n")
        self.put('src/process/pipeline/Canonical.cpp', 'const x = require("../../../api/one.js");\n')
        document, seal = self.context_document()
        self.put('src/process/pipeline/Canonical.cpp', 'const x = require("../../../api/two.js");\n')
        failures = '\n'.join(self.context_failures(document, seal))
        self.assertIn('context_contract:canonical->api/two.js', failures)

    def test_context_packet_includes_real_foreign_sources_and_no_test_offsets(self):
        self.put('src/process/pipeline/Canonical.cpp', 'const x = require("../../../api/package.json");\n')
        document, seal = self.context_document()
        metrics, packets = BUDGET.measure_context(self.root, self.paths(), {},
                                                 document['context_snapshot']['policy'])
        self.assertEqual(packets['canonical']['contract_files'], ['api/package.json'])
        self.assertEqual(packets['canonical']['characters'],
                         metrics['context_characters:canonical'] + metrics['context_characters:canonical/contracts'])
        self.put('test/test_behavior.py', 'necessary evidence\n' * 1000)
        context, _ = BUDGET.measure_context(self.root, self.paths(), {}, document['context_snapshot']['policy'])
        self.assertEqual(context, metrics)
        self.put('src/process/pipeline/Canonical.cpp', 'const x = require(variable);\n')
        self.assertTrue(any('context_dynamic:canonical' in item
                            for item in self.context_failures(document, seal)))

    def test_context_policy_is_sealed_and_deletions_can_only_ratchet_exactly(self):
        document, seal = self.context_document()
        changed = copy.deepcopy(document)
        changed['context_snapshot']['policy']['owners'][0]['patterns'] = ['other/*']
        with self.assertRaisesRegex(ValueError, 'context ownership snapshot changed'):
            BUDGET.limits(changed, self.seal, seal)
        key = 'context_characters:canonical'
        before = document['context_snapshot']['metrics'][key]
        adjustment = {'metric': key, 'from': before, 'to': before - 1,
                      'owner': 'Canonical owner review', 'purpose': 'Remove duplicated implementation',
                      'rejected_reuse': 'Retaining the duplicate is not justified',
                      'cleanup_plan': 'Keep this exact downward ceiling in CI'}
        document['adjustments'].append(adjustment)
        self.assertEqual(BUDGET.limits(document, self.seal, seal)[key], before - 1)
        self.assertTrue(any(key in item for item in self.context_failures(document, seal)))

    def test_context_resolves_supported_literal_import_forms(self):
        paths = {'api/a.js', 'html/js/a.js', 'src/data/A.h', 'script/a.py'}
        cases = [('api/main.js', 'import x from "./a.js";', 'api/a.js'),
                 ('api/main.js', 'import "./a.js";', 'api/a.js'),
                 ('html/index.html', '<script src="/js/a.js?v=1"></script>', 'html/js/a.js'),
                 ('src/main.cpp', '#include "data/A.h"', 'src/data/A.h'),
                 ('src/main.cpp', '#include <data/A.h>', 'src/data/A.h'),
                 ('script/main.py', 'from a import main', 'script/a.py'),
                 ('CMakeLists.txt', '${PROJECT_ROOT}/src/data/A.h', 'src/data/A.h')]
        for path, source, expected in cases:
            with self.subTest(path=path):
                self.assertEqual(BUDGET.local_references(path, source, paths)[0], {expected})

    def test_context_requires_ratchet_and_seals_reviewed_downward_history(self):
        self.approve('lines:src', 0)
        document, seal = self.context_document()
        document['adjustments'].clear()
        with self.assertRaisesRegex(ValueError, 'downward ceilings cannot be removed'):
            BUDGET.limits(document, self.seal, seal)
        self.document['adjustments'].clear()
        document, seal = self.context_document()
        self.put('src/process/pipeline/Canonical.cpp', 'int x;\n')
        self.assertTrue(any('deleted headroom cannot be reused' in item
                            for item in self.context_failures(document, seal)))

    def test_context_grouped_ratchet_remains_exact_and_reviewed(self):
        document, seal = self.context_document()
        adjustment = {'changes': {'lines:src': [1, 0], 'context_lines:canonical': [1, 0]},
                      'owner': 'Canonical owner review', 'purpose': 'Remove duplicated implementation',
                      'rejected_reuse': 'Retaining the duplicate is not justified',
                      'cleanup_plan': 'Keep all exact downward ceilings in CI'}
        document['adjustments'].append(adjustment)
        self.assertEqual(BUDGET.limits(document, self.seal, seal)['lines:src'], 0)
        for changes in ({}, {'lines:*': [1, 0]}, {'lines:src': [2, 0]}, {'lines:src': [1]}):
            adjustment['changes'] = changes
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                BUDGET.limits(document, self.seal, seal)


if __name__ == "__main__":
    unittest.main()
