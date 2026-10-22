import copy
import json
import tempfile
import unittest
from pathlib import Path

from oprs.common import ContractError, load_json
from oprs.contracts import validate_manifest, validate_plan, validate_trial
from oprs.iq import inject_blah2iq, make_fixture, read_blah2iq
from oprs.scoring import score
from oprs.synthetic import conformance_suite


ROOT = Path(__file__).parents[2]
EXAMPLE = ROOT / "examples" / "oprs" / "ref-surv-2ch"


class OprsContractTest(unittest.TestCase):
    def setUp(self):
        self.manifest = load_json(EXAMPLE / "system-manifest.json")
        self.plan = load_json(EXAMPLE / "evaluation-plan.json")
        self.trial = load_json(EXAMPLE / "trial.json")

    def test_reference_example_scores_and_bearing_is_na(self):
        result = score(self.manifest, self.plan, [self.trial])
        self.assertEqual(result["overall_status"], "QUALIFIED")
        self.assertEqual(result["domains"]["bearing_spatial"]["status"], "N/A")
        self.assertEqual(result["mission_tuple"]["tracking_pass_recall"]["value"], 1)

    def test_missing_result_is_a_miss_not_denominator_removal(self):
        trial = copy.deepcopy(self.trial)
        trial["opportunity_results"] = trial["opportunity_results"][:3]
        result = score(self.manifest, self.plan, [trial])
        self.assertEqual(result["evidence_summary"]["missing_results_counted_as_misses"], 3)
        self.assertEqual(result["mission_tuple"]["tracking_pass_recall"]["denominator"], 2)
        self.assertEqual(result["mission_tuple"]["tracking_pass_recall"]["value"], .5)

    def test_duplicate_opportunity_is_rejected(self):
        trial = copy.deepcopy(self.trial)
        trial["opportunity_results"].append(copy.deepcopy(trial["opportunity_results"][0]))
        with self.assertRaisesRegex(ContractError, "duplicate opportunity_id"):
            validate_trial(trial, self.plan)

    def test_duplicate_truth_cpi_is_rejected(self):
        plan = copy.deepcopy(self.plan)
        plan.pop("plan_hash")
        plan["opportunities"][1]["cpi_id"] = plan["opportunities"][0]["cpi_id"]
        with self.assertRaisesRegex(ContractError, "duplicate .*truth opportunity"):
            validate_plan(plan)

    def test_missing_epoch_is_rejected(self):
        trial = copy.deepcopy(self.trial)
        del trial["receiver_epoch_id"]
        with self.assertRaisesRegex(ContractError, "receiver_epoch_id is required"):
            validate_trial(trial, self.plan)

    def test_cpi_time_substitution_is_rejected(self):
        trial = copy.deepcopy(self.trial)
        trial["opportunity_results"][0]["event_time_ns"] += 1
        with self.assertRaisesRegex(ContractError, "timestamp differs"):
            validate_trial(trial, self.plan)

    def test_configuration_cannot_change_opportunity_envelope(self):
        trial = copy.deepcopy(self.trial)
        trial["detector_limits"] = {"delay_min_km": 30}
        result = score(self.manifest, self.plan, [trial])
        self.assertEqual(result["mission_tuple"]["raw_detection_pass_recall"]["denominator"], 2)

    def test_track_claim_needs_confirmed_measurement_identity(self):
        trial = copy.deepcopy(self.trial)
        del trial["opportunity_results"][0]["measurement_id"]
        with self.assertRaisesRegex(ContractError, "measurement_id"):
            validate_trial(trial, self.plan)

    def test_truth_access_by_inference_is_rejected(self):
        trial = copy.deepcopy(self.trial)
        trial["inference_truth_access"] = True
        with self.assertRaisesRegex(ContractError, "truth was available"):
            validate_trial(trial, self.plan)

    def test_invalid_two_channel_roles_are_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["channels"][1]["roles"] = ["reference"]
        with self.assertRaisesRegex(ContractError, "exactly one reference"):
            validate_manifest(manifest)

    def test_suite_has_blanks_staircase_both_signs_and_multipath(self):
        suite = conformance_suite("REF_SURV_2CH")
        self.assertTrue(any(row["blank_control"] for row in suite["scenarios"]))
        dopplers = [point["doppler_hz"] for row in suite["scenarios"] for point in row["trajectory"]]
        self.assertTrue(any(value < 0 for value in dopplers) and any(value > 0 for value in dopplers))
        self.assertTrue(any(row.get("multipath") for row in suite["scenarios"]))

    def test_two_channel_injection_never_changes_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source, output = directory / "source.blah2iq", directory / "output.blah2iq"
            make_fixture(source, sample_rate=20000, seconds=.05)
            scenario = next(row for row in conformance_suite("REF_SURV_2CH")["scenarios"]
                            if row["trajectory"] and row["trajectory"][0]["level_db_ref_rms"] == -36)
            receipt = inject_blah2iq(source, output, self.manifest, scenario)
            _, original = read_blah2iq(source); _, injected = read_blah2iq(output)
            self.assertTrue(receipt["source_channel_unchanged"])
            self.assertEqual(original[0], injected[0])
            self.assertNotEqual(original[1], injected[1])


if __name__ == "__main__":
    unittest.main()
