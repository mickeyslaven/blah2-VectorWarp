"""Offline native-cycle exporter contract; fixtures never qualify live radar."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import vectorwarp_export_cycles as X
import vectorwarp_release_regression as E


class ExportCycles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.ledger = root / "ledger"
        self.ledger.mkdir()
        (root / "archive").mkdir()
        self.archive = E.Evidence(root / "archive", 1)
        self.candidate = dict(sourceCommit="a" * 40, artifactSha256="b" * 64, configSha256="c" * 64)
        self.records = []
        for sequence in range(1, 5):
            def capture(accepted):
                return dict(available=True, epoch="7", discontinuities="0", requalifications="0", retiredCpis="0", retiredSamples="0",
                    lanes=[dict(acceptedSamples=str(accepted), droppedSamples="0", discardedSamples="0")])
            self.records.append(dict(sequence=str(sequence), startMonoNs=str(sequence * 500000000 + 1000000000),
                endMonoNs=str(sequence * 500000000 + 1400000000), drainBatches="0", outcome="completed",
                captureBefore=capture(2**53 + sequence), captureAfter=capture(2**53 + sequence + 1)))

    def write(self, rows=None, **changes):
        records = self.records if rows is None else rows
        raw = b"".join((json.dumps(value) + "\n").encode() for value in records)
        (self.ledger / "cycles.jsonl").write_bytes(raw)
        seal = dict(schema="vectorwarp-cycle-ledger-v1", state="checkpoint", fault="", generation=1, anchorMonoNs="1000000000",
            anchorUtcNs="1795500000000000000", records=len(records), throughSequence=str(len(records)),
            bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        seal.update(changes)
        (self.ledger / "status.json").write_text(json.dumps(seal))

    def export(self, **kwargs):
        return X.export_ledger(self.ledger, self.candidate, self.archive, **kwargs)

    def test_exact_counts_and_existing_evaluator_acceptance(self):
        self.write()
        report = self.export(block_size=2)
        self.assertEqual(len(report["runtimeEvidence"]), 2)
        first = self.archive.load(report["runtimeEvidence"][0])
        self.assertEqual(first["captureStart"]["acceptedSamples"], 2**53 + 1)
        verdict, metrics = E.evaluate_runtime(report["runtimeEvidence"], self.candidate,
            report["startEpoch"], report["endEpoch"], 600, 500, self.archive)
        self.assertTrue(all(verdict.values()))
        self.assertEqual(metrics["completedCpis"], 4)

    def test_missing_duplicate_reset_and_clock_fail(self):
        for change in (lambda rows: rows.pop(1), lambda rows: rows.insert(1, copy.deepcopy(rows[0])),
                lambda rows: rows[1].update(sequence="1"),
                lambda rows: rows[1].update(startMonoNs="1"),
                lambda rows: rows[1]["captureAfter"].update(epoch="8"),
                lambda rows: rows[1]["captureAfter"]["lanes"][0].update(acceptedSamples="1"),
                lambda rows: rows[1]["captureAfter"].update(available=False)):
            with self.subTest(change=change):
                rows = copy.deepcopy(self.records)
                change(rows)
                self.write(rows)
                with self.assertRaises(ValueError):
                    self.export()

    def test_unsealed_fault_digest_and_missing_tail_fail(self):
        for changes in (dict(state="recording"), dict(state="rejected", fault="write-failed"),
                dict(sha256="0" * 64), dict(records=5), dict(bytes=1)):
            with self.subTest(changes=changes):
                self.write(**changes)
                with self.assertRaises(ValueError):
                    self.export()

    def test_counter_overflow_and_rounded_numbers_fail(self):
        for value in (2**53 + 1, str(2**64), "01"):
            rows = copy.deepcopy(self.records)
            rows[0]["captureBefore"]["lanes"][0]["acceptedSamples"] = value
            self.write(rows)
            with self.assertRaises(ValueError):
                self.export()

    def test_pinned_prefix_survives_append_and_checkpoint_replacement(self):
        self.write()
        original_read = X.read_bytes
        def append_after_seal(path):
            raw = original_read(path)
            if path.name == "status.json":
                rows = copy.deepcopy(self.records)
                next_row = copy.deepcopy(rows[-1])
                next_row.update(sequence="5", startMonoNs="3500000000", endMonoNs="3900000000")
                next_row["captureBefore"] = copy.deepcopy(rows[-1]["captureAfter"])
                next_row["captureAfter"]["lanes"][0]["acceptedSamples"] = str(2**53 + 6)
                rows.append(next_row)
                self.write(rows, generation=2)
            return raw
        with patch.object(X, "read_bytes", side_effect=append_after_seal):
            report = self.export()
        block = self.archive.load(report["runtimeEvidence"][0])
        self.assertEqual(block["completedCpisThrough"], 4)
        self.assertEqual(len(block["cycles"]), 4)

    def test_pinned_prefix_rejects_concurrent_truncation_or_corruption(self):
        for tail in (b"", b"{}\n"):
            self.write()
            original_read = X.read_bytes
            def corrupt_after_seal(path):
                raw = original_read(path)
                if path.name == "status.json":
                    (self.ledger / "cycles.jsonl").write_bytes(tail)
                return raw
            with patch.object(X, "read_bytes", side_effect=corrupt_after_seal):
                with self.assertRaises(ValueError):
                    self.export()

    def test_outcomes_and_nonzero_loss_are_retained_not_erased(self):
        for row, outcome in zip(self.records, ("warmup", "rejected", "drain", "final")):
            row["outcome"] = outcome
            row["captureBefore"]["requalifications"] = "1"
            row["captureAfter"]["requalifications"] = "1"
        self.write()
        report = self.export()
        block = self.archive.load(report["runtimeEvidence"][0])
        raw = self.archive.load(block["rawEvidenceSha256"])
        self.assertEqual([row["outcome"] for row in raw["records"]], ["warmup", "rejected", "drain", "final"])
        verdict, _ = E.evaluate_runtime(report["runtimeEvidence"], self.candidate,
            report["startEpoch"], report["endEpoch"], 600, 500, self.archive)
        self.assertFalse(verdict["capture-health"])

    def test_no_advance_cycles_are_retained_across_block_alignments(self):
        for advances in ((1, 1, 1, 1, 0, 0), (0, 1, 0, 0, 1, 0, 0, 0)):
            rows = []
            accepted = 2**53 + 1
            for sequence, advance in enumerate(advances, 1):
                row = copy.deepcopy(self.records[0])
                row.update(sequence=str(sequence), startMonoNs=str(sequence * 500000000 + 1000000000),
                    endMonoNs=str(sequence * 500000000 + 1400000000),
                    outcome="completed" if advance else "final" if sequence == len(advances) else "drain")
                row["captureBefore"]["lanes"][0]["acceptedSamples"] = str(accepted)
                accepted += advance
                row["captureAfter"]["lanes"][0]["acceptedSamples"] = str(accepted)
                rows.append(row)
            self.write(rows)
            for block_size in range(1, len(rows) + 2):
                with self.subTest(advances=advances, block_size=block_size):
                    report = self.export(block_size=block_size)
                    blocks = [self.archive.load(reference) for reference in report["runtimeEvidence"]]
                    self.assertTrue(all(block["captureEnd"]["acceptedSamples"] >
                                        block["captureStart"]["acceptedSamples"] for block in blocks))
                    retained = [row for block in blocks for row in
                                self.archive.load(block["rawEvidenceSha256"])["records"]]
                    self.assertEqual(retained, rows)
                    verdict, metrics = E.evaluate_runtime(report["runtimeEvidence"], self.candidate,
                        report["startEpoch"], report["endEpoch"], 600, 500, self.archive)
                    self.assertTrue(all(verdict.values()))
                    self.assertEqual(metrics["completedCpis"], len(rows))

    def test_entirely_nonadvancing_capture_still_fails(self):
        rows = copy.deepcopy(self.records)
        for row in rows:
            row["captureBefore"]["lanes"][0]["acceptedSamples"] = str(2**53)
            row["captureAfter"]["lanes"][0]["acceptedSamples"] = str(2**53)
        self.write(rows)
        with self.assertRaisesRegex(ValueError, "no accepted sample advance"):
            self.export(block_size=1)

    def test_final_fifo_tail_remains_loss_without_processing_retirement(self):
        rows = copy.deepcopy(self.records)
        final = rows[-1]
        final["outcome"] = "final"
        final["captureAfter"] = copy.deepcopy(final["captureBefore"])
        final["captureAfter"]["lanes"][0].update(discardedSamples="13", droppedSamples="3")
        self.write(rows)
        for block_size in range(1, 6):
            report = self.export(block_size=block_size)
            last = self.archive.load(report["runtimeEvidence"][-1])
            self.assertEqual(last["captureEnd"]["discardedSamples"], 13)
            self.assertEqual(last["captureEnd"]["droppedSamples"], 3)
            self.assertEqual(last["captureEnd"]["retiredSamples"], 0)
            verdict, _ = E.evaluate_runtime(report["runtimeEvidence"], self.candidate,
                report["startEpoch"], report["endEpoch"], 600, 500, self.archive)
            self.assertFalse(verdict["capture-health"])


if __name__ == "__main__":
    unittest.main()
