#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "receiver_updates", ROOT / "script" / "check-receiver-updates.py")
WATCHER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
sys.modules[SPEC.name] = WATCHER
SPEC.loader.exec_module(WATCHER)


def response(url, value):
    body = value if isinstance(value, bytes) else json.dumps(value).encode()
    return WATCHER.FetchResponse(body=body, final_url=url)


class ReceiverUpdateTests(unittest.TestCase):
    def setUp(self):
        self.catalog, self.raw = WATCHER.load_catalog(
            ROOT / "config" / "receiver-software.json")
        self.payloads = {
            "kraken-suite-v2": {
                "sha": "15c1a7ee3f05d909c0c5835addd126849a7d8deb",
                "html_url": "https://github.com/krakenrf/krakensdr_suite/commit/15c1a7ee3f05d909c0c5835addd126849a7d8deb",
            },
            "uhd": {
                "tag_name": "v4.10.0.0", "draft": False, "prerelease": False,
                "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.10.0.0",
                "published_at": "2026-04-27T17:13:00Z",
            },
            "libhackrf": {
                "tag_name": "v2026.01.3", "draft": False, "prerelease": False,
                "html_url": "https://github.com/greatscottgadgets/hackrf/releases/tag/v2026.01.3",
                "published_at": "2026-01-30T04:02:00Z",
            },
            "sdrplay-api": b"<html><title>Hardware API Linux</title><div>3.15 499 KB</div></html>",
        }
        self.commits = {
            "uhd": "2af4ddb96219a99d2300804830e0971f79557b23",
            "libhackrf": "1cfe7dfe98d333450217d50e3f3a1ad0702e000f",
        }

    def fetcher(self, url, identifier):
        if identifier in self.commits and "/commits/" in url:
            commit = self.commits[identifier]
            repository = WATCHER.APPROVED[identifier]["repository"]
            return response(url, {
                "sha": commit,
                "html_url": f"https://github.com/{repository}/commit/{commit}",
            })
        return response(url, self.payloads[identifier])

    def test_redirect_is_rejected_before_forwarding_authorization(self):
        request = WATCHER.urllib.request.Request(
            'https://api.github.com/repos/EttusResearch/uhd/releases/latest',
            headers={'Authorization': 'Bearer fixture-not-a-token'})
        redirects = WATCHER.ApprovedRedirects('uhd')
        for target in ['https://example.net/capture', 'http://api.github.com/capture',
                       'https://api.github.com.evil.example/capture']:
            with self.assertRaises(WATCHER.WatcherError):
                redirects.redirect_request(request, None, 302, 'Found', {}, target)
        accepted = redirects.redirect_request(request, None, 302, 'Found', {},
                                               'https://api.github.com/same-host')
        self.assertEqual(accepted.host, 'api.github.com')

    def test_current_official_versions_are_unchanged_without_compatibility_claim(self):
        report = WATCHER.check_catalog(
            self.catalog, self.raw, self.fetcher, "2026-09-10T20:00:00Z")
        self.assertEqual(report["summary"], {
            "sources": 4, "updates": 0, "unchanged": 4, "errors": 0,
            "hasUpdates": False, "complete": True,
        })
        self.assertEqual(report["openSourceBuildMatrix"], {"include": []})
        for entry in report["entries"]:
            self.assertIsNone(entry["compatibility"]["compatible"])
            self.assertFalse(entry["compatibility"]["hardwareValidated"])
            self.assertEqual(entry["compatibility"]["state"],
                             "not-run-no-new-version")

    def test_updates_schedule_only_approved_open_source_candidate_builds(self):
        self.payloads["kraken-suite-v2"] = {
            "sha": "a" * 40,
            "html_url": "https://github.com/krakenrf/krakensdr_suite/commit/" + "a" * 40,
        }
        self.payloads["uhd"] = {
            "tag_name": "v4.11.0.0", "draft": False, "prerelease": False,
            "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.11.0.0",
            "published_at": "2026-09-11T00:00:00Z",
        }
        self.commits["uhd"] = "b" * 40
        self.payloads["sdrplay-api"] = (
            b"<html><h1>Hardware API Linux</h1><div>3.16 510 KB</div></html>")
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher,
                                       "2026-09-11T01:00:00Z")
        self.assertEqual(report["summary"]["updates"], 3)
        matrix = report["openSourceBuildMatrix"]["include"]
        self.assertEqual(len(matrix), 1)
        self.assertEqual(matrix[0], {
            "id": "uhd", "repository": "EttusResearch/uhd",
            "commit": "b" * 40, "identity": "v4.11.0.0",
            "version": "4.11.0.0", "backend": "usrp",
        })
        entries = {item["id"]: item for item in report["entries"]}
        self.assertEqual(entries["kraken-suite-v2"]["compatibility"]["state"],
                         "pending-manual-protocol-review")
        self.assertEqual(entries["sdrplay-api"]["compatibility"]["state"],
                         "pending-manual-licensed-build")
        self.assertFalse(entries["sdrplay-api"]["licensePolicy"]["automaticSourceCheckout"])
        self.assertFalse(entries["sdrplay-api"]["licensePolicy"]["automaticVendorInstall"])

    def test_release_tag_move_is_a_new_immutable_candidate(self):
        self.commits["uhd"] = "d" * 40
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher,
                                       "2026-09-11T01:00:00Z")
        self.assertEqual(report["summary"]["updates"], 1)
        self.assertEqual(report["openSourceBuildMatrix"]["include"][0]["commit"], "d" * 40)

    def test_bad_source_result_is_an_incomplete_report_not_false_absence(self):
        self.payloads["uhd"] = {
            "tag_name": "v4.11.0.0-rc1", "draft": False, "prerelease": True,
            "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.11.0.0-rc1",
        }
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher)
        self.assertEqual(report["summary"]["errors"], 1)
        self.assertFalse(report["summary"]["complete"])
        uhd = next(item for item in report["entries"] if item["id"] == "uhd")
        self.assertEqual(uhd["status"], "error")
        self.assertIsNone(uhd["latest"])
        self.assertIsNone(uhd["compatibility"]["compatible"])

    def test_catalog_cannot_add_an_arbitrary_repository_or_enable_vendor_download(self):
        alterations = []
        repository = json.loads(self.raw)
        repository["software"][1]["source"]["repository"] = "attacker/uhd"
        alterations.append((repository, "approved source"))
        vendor_install = json.loads(self.raw)
        vendor_install["software"][3]["licensePolicy"]["automaticVendorInstall"] = True
        alterations.append((vendor_install, "licensePolicy"))
        mutable_release = json.loads(self.raw)
        mutable_release["software"][1]["lastSeen"].pop("sourceCommit")
        alterations.append((mutable_release, "sourceCommit"))
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "catalog.json"
            for altered, message in alterations:
                with self.subTest(message=message):
                    path.write_text(json.dumps(altered))
                    with self.assertRaisesRegex(WATCHER.WatcherError, message):
                        WATCHER.load_catalog(path)

    def test_issue_rendering_distinguishes_compile_from_hardware_validation(self):
        self.payloads["uhd"] = {
            "tag_name": "v4.11.0.0", "draft": False, "prerelease": False,
            "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.11.0.0",
            "published_at": "2026-09-11T00:00:00Z",
        }
        self.commits["uhd"] = "b" * 40
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher)
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            WATCHER._write_json(base / "report.json", report)
            WATCHER._write_json(base / "candidates" / "uhd.json", {
                "schemaVersion": 1, "id": "uhd", "identity": "v4.11.0.0",
                "sourceCommit": "b" * 40, "vectorwarpCommit": "c" * 40,
                "backend": "usrp", "status": "passed",
                "compiledReceivers": "Usrp,Kraken", "hardwareValidated": False,
            })
            count = WATCHER.render_issue_descriptors(
                base / "report.json", base / "candidates", base / "issues",
                "https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/123")
            self.assertEqual(count, 1)
            issue = json.loads((base / "issues" / "uhd.json").read_text())
            self.assertIn("source-level compile evidence only", issue["body"])
            self.assertIn("no receiver hardware was tested", issue["body"])
            self.assertIn("Tested source commit: `" + "b" * 40 + "`", issue["body"])
            self.assertIn("Tested VectorWarp commit: `" + "c" * 40 + "`", issue["body"])
            self.assertIn("No user host, deployed package", issue["body"])
            self.assertRegex(issue["marker"], r"^receiver-update-[0-9a-f]{24}$")

    def test_malformed_candidate_cannot_inject_issue_content(self):
        self.payloads["uhd"] = {
            "tag_name": "v4.11.0.0", "draft": False, "prerelease": False,
            "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.11.0.0",
            "published_at": None,
        }
        self.commits["uhd"] = "b" * 40
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher)
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            WATCHER._write_json(base / "report.json", report)
            WATCHER._write_json(base / "candidates" / "uhd.json", {
                "schemaVersion": 1, "id": "uhd", "identity": "v4.11.0.0",
                "sourceCommit": "b" * 40, "vectorwarpCommit": "c" * 40,
                "backend": "usrp", "status": "passed",
                "compiledReceivers": "$(touch /tmp/no)", "hardwareValidated": False,
            })
            with self.assertRaisesRegex(WATCHER.WatcherError, "build contract"):
                WATCHER.render_issue_descriptors(
                    base / "report.json", base / "candidates", base / "issues",
                    "https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/123")

    def test_candidate_result_must_match_discovered_commit(self):
        self.payloads["uhd"] = {
            "tag_name": "v4.11.0.0", "draft": False, "prerelease": False,
            "html_url": "https://github.com/EttusResearch/uhd/releases/tag/v4.11.0.0",
            "published_at": None,
        }
        self.commits["uhd"] = "b" * 40
        report = WATCHER.check_catalog(self.catalog, self.raw, self.fetcher)
        with tempfile.TemporaryDirectory() as directory:
            base = pathlib.Path(directory)
            WATCHER._write_json(base / "report.json", report)
            WATCHER._write_json(base / "candidates" / "uhd.json", {
                "schemaVersion": 1, "id": "uhd", "identity": "v4.11.0.0",
                "sourceCommit": "e" * 40, "vectorwarpCommit": "c" * 40,
                "backend": "usrp", "status": "passed",
                "compiledReceivers": "Usrp,Kraken", "hardwareValidated": False,
            })
            with self.assertRaisesRegex(WATCHER.WatcherError, "discovered source commit"):
                WATCHER.render_issue_descriptors(
                    base / "report.json", base / "candidates", base / "issues",
                    "https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/123")


if __name__ == "__main__":
    unittest.main()
