#!/usr/bin/env python3
"""Bounded, read-only release discovery for VectorWarp receiver software."""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import html.parser
import json
import os
import pathlib
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable

MAX_CATALOG_BYTES = 128 * 1024
MAX_RESPONSE_BYTES = 1024 * 1024
MAX_REPORT_BYTES = 512 * 1024
USER_AGENT = "VectorWarp-receiver-compatibility/1"
COMMIT_IDENTITY = re.compile(r"^[0-9a-f]{40}$")

APPROVED = {
    "kraken-suite-v2": {
        "displayName": "KrakenSDR Suite V2",
        "receiverType": "Kraken",
        "kind": "github-branch",
        "repository": "krakenrf/krakensdr_suite",
        "branch": "main",
        "candidateCheck": "manual-protocol-review",
        "backend": "kraken",
        "automaticSourceCheckout": False,
        "identity": re.compile(r"^[0-9a-f]{40}$"),
    },
    "uhd": {
        "displayName": "Ettus Research UHD",
        "receiverType": "Usrp",
        "kind": "github-release",
        "repository": "EttusResearch/uhd",
        "candidateCheck": "source-build",
        "backend": "usrp",
        "automaticSourceCheckout": True,
        "identity": re.compile(r"^v\d+\.\d+\.\d+\.\d+$"),
    },
    "libhackrf": {
        "displayName": "Great Scott Gadgets HackRF",
        "receiverType": "HackRF",
        "kind": "github-release",
        "repository": "greatscottgadgets/hackrf",
        "candidateCheck": "source-build",
        "backend": "hackrf",
        "automaticSourceCheckout": True,
        "identity": re.compile(r"^v\d{4}\.\d{2}\.\d+$"),
    },
    "sdrplay-api": {
        "displayName": "SDRplay Hardware API for Linux",
        "receiverType": "RspDuo",
        "kind": "sdrplay-download-page",
        "url": "https://sdrplay.com/download/hardware-api-linux/",
        "candidateCheck": "manual-licensed-build",
        "backend": "rspduo",
        "automaticSourceCheckout": False,
        "identity": re.compile(r"^3\.\d{1,3}(?:\.\d{1,3})?$"),
    },
}


class WatcherError(Exception):
    """A bounded validation or discovery failure safe to summarize."""


@dataclasses.dataclass(frozen=True)
class FetchResponse:
    body: bytes
    final_url: str


class _VisibleText(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self.hidden_depth = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        del attrs
        if tag.lower() in {"script", "style", "noscript"}:
            self.hidden_depth += 1

    def handle_endtag(self, tag: str) -> None:
        if tag.lower() in {"script", "style", "noscript"} and self.hidden_depth:
            self.hidden_depth -= 1

    def handle_data(self, data: str) -> None:
        if not self.hidden_depth:
            self.parts.append(data)


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise WatcherError(f"JSON contains duplicate field {key}")
        result[key] = value
    return result


def _decode_json(raw: bytes, name: str) -> object:
    try:
        return json.loads(raw, object_pairs_hook=_reject_duplicate_keys)
    except WatcherError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise WatcherError(f"{name} is not valid UTF-8 JSON") from error


def _exact_keys(value: object, allowed: set[str], name: str) -> dict:
    if not isinstance(value, dict):
        raise WatcherError(f"{name} must be an object")
    extra = set(value) - allowed
    if extra:
        raise WatcherError(f"{name} has unsupported field {sorted(extra)[0]}")
    return value


def _short_text(value: object, name: str, limit: int = 160) -> str:
    if not isinstance(value, str) or not value or len(value) > limit:
        raise WatcherError(f"{name} is invalid")
    if any(ord(character) < 32 or ord(character) == 127 for character in value):
        raise WatcherError(f"{name} contains control characters")
    return value


def _read_bounded(path: pathlib.Path, limit: int, name: str) -> bytes:
    try:
        with path.open("rb") as source:
            content = source.read(limit + 1)
    except OSError as error:
        raise WatcherError(f"Could not read {name}: {error.strerror or error.__class__.__name__}") from error
    if len(content) > limit:
        raise WatcherError(f"{name} exceeds {limit} bytes")
    return content


def load_catalog(path: pathlib.Path) -> tuple[dict, bytes]:
    raw = _read_bounded(path, MAX_CATALOG_BYTES, "receiver software catalog")
    catalog = _decode_json(raw, "Receiver software catalog")
    _exact_keys(catalog, {"schemaVersion", "software"}, "catalog")
    if catalog.get("schemaVersion") != 1:
        raise WatcherError("catalog.schemaVersion must be 1")
    software = catalog.get("software")
    if not isinstance(software, list) or len(software) != len(APPROVED):
        raise WatcherError("catalog.software must contain the four approved sources")
    seen: set[str] = set()
    for index, entry in enumerate(software):
        name = f"catalog.software[{index}]"
        _exact_keys(entry, {"id", "displayName", "receiverType", "source", "lastSeen",
                            "compatibility", "licensePolicy"}, name)
        identifier = _short_text(entry.get("id"), f"{name}.id", 40)
        if identifier not in APPROVED or identifier in seen:
            raise WatcherError(f"{name}.id is not a unique approved source")
        seen.add(identifier)
        approved = APPROVED[identifier]
        for field in ("displayName", "receiverType"):
            if entry.get(field) != approved[field]:
                raise WatcherError(f"{name}.{field} does not match the approved source")
        source = _exact_keys(entry.get("source"),
                             {"kind", "repository", "branch", "url"}, f"{name}.source")
        if source.get("kind") != approved["kind"]:
            raise WatcherError(f"{name}.source.kind does not match the approved source")
        for field in ("repository", "branch", "url"):
            if field in approved and source.get(field) != approved[field]:
                raise WatcherError(f"{name}.source.{field} does not match the approved source")
            if field not in approved and field in source:
                raise WatcherError(f"{name}.source.{field} is not allowed for this source")
        last_seen = _exact_keys(entry.get("lastSeen"), {"identity", "version", "sourceCommit"},
                                f"{name}.lastSeen")
        identity = _short_text(last_seen.get("identity"), f"{name}.lastSeen.identity")
        version = _short_text(last_seen.get("version"), f"{name}.lastSeen.version")
        if not approved["identity"].fullmatch(identity):
            raise WatcherError(f"{name}.lastSeen.identity has an invalid format")
        expected_version = (f"{approved['branch']}@{identity[:12]}"
                            if approved["kind"] == "github-branch"
                            else identity.removeprefix("v"))
        if version != expected_version:
            raise WatcherError(f"{name}.lastSeen.version does not match its identity")
        source_commit = last_seen.get("sourceCommit")
        if approved["kind"] == "github-release":
            source_commit = _short_text(source_commit, f"{name}.lastSeen.sourceCommit", 40)
            if not COMMIT_IDENTITY.fullmatch(source_commit):
                raise WatcherError(f"{name}.lastSeen.sourceCommit is not an immutable commit")
        elif source_commit is not None:
            raise WatcherError(f"{name}.lastSeen.sourceCommit is only allowed for release tags")
        compatibility = _exact_keys(entry.get("compatibility"),
                                    {"candidateCheck", "backend"}, f"{name}.compatibility")
        if compatibility != {"candidateCheck": approved["candidateCheck"],
                              "backend": approved["backend"]}:
            raise WatcherError(f"{name}.compatibility does not match the approved policy")
        license_policy = _exact_keys(entry.get("licensePolicy"),
                                     {"automaticSourceCheckout", "automaticVendorInstall"},
                                     f"{name}.licensePolicy")
        if license_policy != {
            "automaticSourceCheckout": approved["automaticSourceCheckout"],
            "automaticVendorInstall": False,
        }:
            raise WatcherError(f"{name}.licensePolicy does not match the approved policy")
    if seen != set(APPROVED):
        raise WatcherError("catalog.software is missing an approved source")
    return catalog, raw


def source_url(entry: dict) -> str:
    source = entry["source"]
    if source["kind"] == "github-release":
        return f"https://api.github.com/repos/{source['repository']}/releases/latest"
    if source["kind"] == "github-branch":
        return f"https://api.github.com/repos/{source['repository']}/commits/{source['branch']}"
    return source["url"]


def _allowed_final_url(identifier: str, url: str) -> bool:
    parsed = urllib.parse.urlsplit(url)
    try:
        port = parsed.port
    except ValueError:
        return False
    if parsed.scheme != "https" or parsed.username or parsed.password or port not in (None, 443):
        return False
    host = (parsed.hostname or "").lower()
    if identifier == "sdrplay-api":
        return host in {"sdrplay.com", "www.sdrplay.com"}
    return host == "api.github.com"


class ApprovedRedirects(urllib.request.HTTPRedirectHandler):
    """Validate BEFORE following a redirect or forwarding its headers."""

    def __init__(self, identifier: str) -> None:
        self.identifier = identifier

    def redirect_request(self, request, fp, code, message, headers, newurl):
        if not _allowed_final_url(self.identifier, newurl):
            raise WatcherError("Source redirected outside the approved host")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def http_fetch(url: str, identifier: str, timeout: float = 10.0) -> FetchResponse:
    if not _allowed_final_url(identifier, url):
        raise WatcherError("Source URL is outside the approved host")
    headers = {"Accept": "application/vnd.github+json", "User-Agent": USER_AGENT}
    token = os.environ.get("GH_TOKEN", "")
    if token and urllib.parse.urlsplit(url).hostname == "api.github.com":
        headers["Authorization"] = f"Bearer {token}"
        headers["X-GitHub-Api-Version"] = "2022-11-28"
    request = urllib.request.Request(url, headers=headers)
    try:
        opener = urllib.request.build_opener(ApprovedRedirects(identifier))
        with opener.open(request, timeout=timeout) as response:
            final_url = response.geturl()
            if not _allowed_final_url(identifier, final_url):
                raise WatcherError("Source redirected outside the approved host")
            body = response.read(MAX_RESPONSE_BYTES + 1)
    except WatcherError:
        raise
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise WatcherError("Official source request failed") from error
    if len(body) > MAX_RESPONSE_BYTES:
        raise WatcherError("Official source response exceeds 1 MiB")
    return FetchResponse(body=body, final_url=final_url)


def _github_json(response: FetchResponse, name: str) -> dict:
    value = _decode_json(response.body, name)
    if not isinstance(value, dict):
        raise WatcherError(f"{name} returned the wrong JSON shape")
    return value


def discover_latest(entry: dict, fetcher: Callable[[str, str], FetchResponse]) -> dict:
    identifier = entry["id"]
    approved = APPROVED[identifier]
    response = fetcher(source_url(entry), identifier)
    if not isinstance(response, FetchResponse) or len(response.body) > MAX_RESPONSE_BYTES:
        raise WatcherError("Fetcher returned an invalid or oversized response")
    if not _allowed_final_url(identifier, response.final_url):
        raise WatcherError("Fetcher returned an unapproved final URL")
    if entry["source"]["kind"] == "github-release":
        value = _github_json(response, entry["displayName"])
        if value.get("draft") is not False or value.get("prerelease") is not False:
            raise WatcherError("GitHub latest release is not a stable published release")
        identity = _short_text(value.get("tag_name"), "release tag", 80)
        if not approved["identity"].fullmatch(identity):
            raise WatcherError("Latest release tag has an unexpected format")
        page = _short_text(value.get("html_url"), "release URL", 300)
        expected_page = f"https://github.com/{entry['source']['repository']}/releases/tag/{identity}"
        if page != expected_page:
            raise WatcherError("Latest release page is outside the approved repository")
        published = value.get("published_at")
        if published is not None:
            published = _short_text(published, "release publication time", 64)
        commit_api = (f"https://api.github.com/repos/{entry['source']['repository']}/commits/"
                      f"{urllib.parse.quote(identity, safe='')}")
        commit_response = fetcher(commit_api, identifier)
        if not isinstance(commit_response, FetchResponse) or \
                len(commit_response.body) > MAX_RESPONSE_BYTES or \
                not _allowed_final_url(identifier, commit_response.final_url):
            raise WatcherError("Release commit lookup returned an invalid response")
        commit_value = _github_json(commit_response, f"{entry['displayName']} release commit")
        source_commit = _short_text(commit_value.get("sha"), "release source commit", 40)
        if not COMMIT_IDENTITY.fullmatch(source_commit):
            raise WatcherError("Release tag did not resolve to an immutable commit")
        commit_page = _short_text(commit_value.get("html_url"), "release commit URL", 300)
        expected_commit_page = f"https://github.com/{entry['source']['repository']}/commit/{source_commit}"
        if commit_page != expected_commit_page:
            raise WatcherError("Release commit page is outside the approved repository")
        return {"identity": identity, "version": identity.removeprefix("v"),
                "sourceCommit": source_commit, "url": page, "publishedAt": published}
    if entry["source"]["kind"] == "github-branch":
        value = _github_json(response, entry["displayName"])
        identity = _short_text(value.get("sha"), "branch commit", 40)
        if not approved["identity"].fullmatch(identity):
            raise WatcherError("Branch response has an invalid commit identity")
        page = _short_text(value.get("html_url"), "commit URL", 300)
        expected = f"https://github.com/{entry['source']['repository']}/commit/{identity}"
        if page != expected:
            raise WatcherError("Branch response commit page is outside the approved repository")
        return {"identity": identity,
                "version": f"{entry['source']['branch']}@{identity[:12]}",
                "url": page, "publishedAt": None}
    try:
        page = response.body.decode("utf-8")
    except UnicodeDecodeError as error:
        raise WatcherError("SDRplay download page is not UTF-8") from error
    if "Hardware API Linux" not in page:
        raise WatcherError("SDRplay page no longer identifies the Hardware API for Linux")
    visible_parser = _VisibleText()
    visible_parser.feed(page)
    visible = " ".join(" ".join(visible_parser.parts).split())
    versions = re.findall(
        r"\b(3\.\d{1,3}(?:\.\d{1,3})?)\b(?=.{0,80}\b\d+(?:\.\d+)?\s*(?:KB|MB)\b)",
        visible, flags=re.IGNORECASE)
    versions = [version for version in versions if approved["identity"].fullmatch(version)]
    if not versions:
        raise WatcherError("Could not identify a bounded SDRplay API version")
    version = max(versions, key=lambda item: tuple(int(part) for part in item.split(".")))
    return {"identity": version, "version": version,
            "url": entry["source"]["url"], "publishedAt": None}


def _compatibility(entry: dict, changed: bool) -> dict:
    check = entry["compatibility"]["candidateCheck"]
    if not changed:
        state = "not-run-no-new-version"
    elif check == "source-build":
        state = "pending-candidate-source-build"
    elif check == "manual-protocol-review":
        state = "pending-manual-protocol-review"
    else:
        state = "pending-manual-licensed-build"
    return {
        "state": state,
        "candidateCheck": check,
        "backend": entry["compatibility"]["backend"],
        "compatible": None,
        "hardwareValidated": False,
    }


def check_catalog(catalog: dict, raw_catalog: bytes,
                  fetcher: Callable[[str, str], FetchResponse] = http_fetch,
                  checked_at: str | None = None) -> dict:
    entries = []
    open_candidates = []
    for entry in catalog["software"]:
        try:
            latest = discover_latest(entry, fetcher)
            changed = latest["identity"] != entry["lastSeen"]["identity"]
            if entry["source"]["kind"] == "github-release":
                changed = changed or latest["sourceCommit"] != entry["lastSeen"]["sourceCommit"]
            status = "update-available" if changed else "unchanged"
            result = {
                "id": entry["id"], "displayName": entry["displayName"],
                "receiverType": entry["receiverType"], "status": status,
                "lastSeen": entry["lastSeen"], "latest": latest,
                "compatibility": _compatibility(entry, changed),
                "licensePolicy": entry["licensePolicy"],
            }
            entries.append(result)
            if changed and entry["compatibility"]["candidateCheck"] == "source-build":
                open_candidates.append({
                    "id": entry["id"],
                    "repository": entry["source"]["repository"],
                    "commit": latest["sourceCommit"],
                    "identity": latest["identity"],
                    "version": latest["version"],
                    "backend": entry["compatibility"]["backend"],
                })
        except WatcherError as error:
            entries.append({
                "id": entry["id"], "displayName": entry["displayName"],
                "receiverType": entry["receiverType"], "status": "error",
                "lastSeen": entry["lastSeen"], "latest": None,
                "compatibility": _compatibility(entry, False),
                "licensePolicy": entry["licensePolicy"],
                "error": str(error)[:240],
            })
    updates = sum(entry["status"] == "update-available" for entry in entries)
    errors = sum(entry["status"] == "error" for entry in entries)
    moment = checked_at or dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", moment):
        raise WatcherError("checked_at must be a UTC second timestamp")
    report = {
        "schemaVersion": 1,
        "checkedAt": moment,
        "catalogSha256": hashlib.sha256(raw_catalog).hexdigest(),
        "summary": {"sources": len(entries), "updates": updates,
                    "unchanged": len(entries) - updates - errors, "errors": errors,
                    "hasUpdates": updates > 0, "complete": errors == 0},
        "entries": entries,
        "openSourceBuildMatrix": {"include": open_candidates},
    }
    encoded = json.dumps(report, sort_keys=True, separators=(",", ":")).encode()
    if len(encoded) > MAX_REPORT_BYTES:
        raise WatcherError("Receiver update report exceeds its size limit")
    return report


def _write_json(path: pathlib.Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(value, indent=2, sort_keys=True) + "\n"
    path.write_text(encoded, encoding="utf-8")


def _write_github_outputs(path: pathlib.Path, report: dict) -> None:
    matrix = json.dumps(report["openSourceBuildMatrix"], separators=(",", ":"))
    if "\n" in matrix or len(matrix) > 8192:
        raise WatcherError("Candidate build matrix is invalid or oversized")
    with path.open("a", encoding="utf-8") as output:
        output.write(f"has_updates={'true' if report['summary']['hasUpdates'] else 'false'}\n")
        output.write(f"has_open_candidates={'true' if report['openSourceBuildMatrix']['include'] else 'false'}\n")
        output.write(f"open_matrix={matrix}\n")


def _load_report(path: pathlib.Path) -> dict:
    raw = _read_bounded(path, MAX_REPORT_BYTES, "receiver update report")
    report = _decode_json(raw, "Receiver update report")
    _exact_keys(report, {"schemaVersion", "checkedAt", "catalogSha256", "summary",
                         "entries", "openSourceBuildMatrix"}, "report")
    if report.get("schemaVersion") != 1 or not isinstance(report.get("entries"), list):
        raise WatcherError("Receiver update report has the wrong schema")
    return report


def _candidate_results(directory: pathlib.Path) -> dict[tuple[str, str], dict]:
    results: dict[tuple[str, str], dict] = {}
    if not directory.exists():
        return results
    for path in sorted(directory.glob("*.json"))[:8]:
        raw = _read_bounded(path, 16 * 1024, "candidate result")
        value = _decode_json(raw, "Candidate result")
        _exact_keys(value, {"schemaVersion", "id", "identity", "sourceCommit",
                            "vectorwarpCommit", "backend", "status", "compiledReceivers",
                            "hardwareValidated"}, "candidate result")
        if value.get("schemaVersion") != 1 or value.get("id") not in {"uhd", "libhackrf"}:
            raise WatcherError("Candidate result has an unsupported identity")
        identity = _short_text(value.get("identity"), "candidate identity", 80)
        if not APPROVED[value["id"]]["identity"].fullmatch(identity):
            raise WatcherError("Candidate result identity is malformed")
        source_commit = _short_text(value.get("sourceCommit"), "candidate source commit", 40)
        vectorwarp_commit = _short_text(value.get("vectorwarpCommit"),
                                        "candidate VectorWarp commit", 40)
        if not COMMIT_IDENTITY.fullmatch(source_commit) or \
                not COMMIT_IDENTITY.fullmatch(vectorwarp_commit):
            raise WatcherError("Candidate result commit identity is malformed")
        expected_receivers = {"uhd": "Usrp,Kraken", "libhackrf": "HackRF,Kraken"}
        if value.get("backend") != APPROVED[value["id"]]["backend"] or \
                value.get("compiledReceivers") != expected_receivers[value["id"]]:
            raise WatcherError("Candidate result does not match its approved build contract")
        if value.get("status") not in {"passed", "failed"} or value.get("hardwareValidated") is not False:
            raise WatcherError("Candidate result status is invalid")
        key = (value["id"], identity)
        if key in results:
            raise WatcherError("Candidate results contain a duplicate identity")
        results[key] = value
    return results


def render_issue_descriptors(report_path: pathlib.Path, candidate_directory: pathlib.Path,
                             output_directory: pathlib.Path, run_url: str) -> int:
    report = _load_report(report_path)
    if not re.fullmatch(r"https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/actions/runs/\d+", run_url):
        raise WatcherError("Workflow run URL is invalid")
    candidates = _candidate_results(candidate_directory)
    output_directory.mkdir(parents=True, exist_ok=True)
    count = 0
    for entry in report["entries"]:
        if entry.get("status") != "update-available":
            continue
        identifier = entry.get("id")
        if identifier not in APPROVED:
            raise WatcherError("Report contains an unapproved software identity")
        approved = APPROVED[identifier]
        if entry.get("displayName") != approved["displayName"] or \
                entry.get("receiverType") != approved["receiverType"]:
            raise WatcherError("Report source identity does not match the approved catalog")
        last_seen = entry.get("lastSeen")
        if not isinstance(last_seen, dict):
            raise WatcherError("Report source has no catalog identity")
        old_identity = _short_text(last_seen.get("identity"), "catalog identity", 80)
        if not approved["identity"].fullmatch(old_identity):
            raise WatcherError("Report catalog identity is malformed")
        latest = entry.get("latest")
        if not isinstance(latest, dict):
            raise WatcherError("Updated report entry has no latest version")
        identity = _short_text(latest.get("identity"), "latest identity", 80)
        if not APPROVED[identifier]["identity"].fullmatch(identity):
            raise WatcherError("Updated report identity is malformed")
        version = _short_text(latest.get("version"), "latest version", 80)
        expected_version = (f"{approved['branch']}@{identity[:12]}"
                            if approved["kind"] == "github-branch"
                            else identity.removeprefix("v"))
        if version != expected_version:
            raise WatcherError("Report latest version does not match its identity")
        source_commit = latest.get("sourceCommit")
        if approved["kind"] == "github-release":
            source_commit = _short_text(source_commit, "latest source commit", 40)
            if not COMMIT_IDENTITY.fullmatch(source_commit):
                raise WatcherError("Report release commit identity is malformed")
        elif source_commit is not None:
            raise WatcherError("Report source commit is unexpected for this source")
        latest_url = _short_text(latest.get("url"), "latest source URL", 300)
        parsed_latest = urllib.parse.urlsplit(latest_url)
        if parsed_latest.scheme != "https" or (parsed_latest.hostname or "").lower() not in {
                "github.com", "sdrplay.com", "www.sdrplay.com"}:
            raise WatcherError("Report latest source URL is not approved")
        if approved["kind"] == "github-release":
            expected_prefix = f"https://github.com/{approved['repository']}/releases/tag/"
            if not latest_url.startswith(expected_prefix):
                raise WatcherError("Report release URL does not match its approved repository")
        elif approved["kind"] == "github-branch":
            expected_url = f"https://github.com/{approved['repository']}/commit/{identity}"
            if latest_url != expected_url:
                raise WatcherError("Report commit URL does not match its approved repository")
        elif latest_url != approved["url"]:
            raise WatcherError("Report vendor URL does not match its approved page")
        title = f"Receiver compatibility review: {entry['displayName']} {version}"
        fingerprint = hashlib.sha256(
            f"{identifier}\0{identity}\0{source_commit or ''}".encode()).hexdigest()[:24]
        marker = f"receiver-update-{fingerprint}"
        candidate = candidates.get((identifier, identity))
        compatibility_result = entry.get("compatibility")
        if not isinstance(compatibility_result, dict) or \
                compatibility_result.get("candidateCheck") != approved["candidateCheck"] or \
                compatibility_result.get("backend") != approved["backend"]:
            raise WatcherError("Report compatibility policy does not match the approved source")
        check = compatibility_result["candidateCheck"]
        if candidate and candidate["sourceCommit"] != source_commit:
            raise WatcherError("Candidate result does not match the discovered source commit")
        if candidate and candidate["status"] == "passed":
            compatibility = (
                f"The guarded candidate job compiled the `{candidate['backend']}` backend and produced "
                f"`{candidate['compiledReceivers']}`. This is source-level compile evidence only; no "
                "receiver hardware was tested."
            )
        elif candidate and candidate["status"] == "failed":
            compatibility = "The guarded candidate source build failed. Compatibility is not established."
        elif check == "manual-licensed-build":
            compatibility = (
                "Compatibility is pending an operator-supplied licensed SDK build. Automation did not "
                "download, accept, install, or redistribute the vendor SDK."
            )
        elif check == "manual-protocol-review":
            compatibility = (
                "Compatibility is pending a manual Heimdall protocol/source review. Branch discovery "
                "does not prove control or stream compatibility."
            )
        else:
            compatibility = "Candidate source-build evidence is unavailable. Compatibility remains pending."
        body = "\n".join([
            f"<!-- {marker} -->",
            "A newer approved receiver-software source was observed by the guarded watcher.",
            "",
            f"- Software: {entry['displayName']}",
            f"- Receiver: `{entry['receiverType']}`",
            f"- Catalog identity: `{old_identity}`",
            f"- Observed identity: `{identity}`",
            *([f"- Tested source commit: `{source_commit}`"] if source_commit else []),
            *([f"- Tested VectorWarp commit: `{candidate['vectorwarpCommit']}`"]
              if candidate else []),
            f"- Official source: {latest_url}",
            f"- Workflow run: {run_url}",
            "",
            compatibility,
            "",
            "No user host, deployed package, service, release, configuration, firmware, or receiver was changed automatically.",
            "After review, update the catalog `lastSeen` value in a normal pull request to acknowledge this version.",
            "",
        ])
        descriptor = {"schemaVersion": 1, "id": identifier, "identity": identity,
                      "title": title, "marker": marker, "body": body}
        _write_json(output_directory / f"{identifier}.json", descriptor)
        count += 1
    return count


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    check = subparsers.add_parser("check", help="query only the approved official sources")
    check.add_argument("--catalog", type=pathlib.Path,
                       default=pathlib.Path("config/receiver-software.json"))
    check.add_argument("--output", type=pathlib.Path, required=True)
    check.add_argument("--github-output", type=pathlib.Path)
    check.add_argument("--checked-at")
    check.add_argument("--fail-on-errors", action="store_true")
    render = subparsers.add_parser("render-issues", help="render bounded issue descriptors")
    render.add_argument("--report", type=pathlib.Path, required=True)
    render.add_argument("--candidate-results", type=pathlib.Path, required=True)
    render.add_argument("--output-dir", type=pathlib.Path, required=True)
    render.add_argument("--run-url", required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    try:
        if args.command == "check":
            catalog, raw = load_catalog(args.catalog)
            if args.checked_at and not re.fullmatch(
                    r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", args.checked_at):
                raise WatcherError("--checked-at must be a UTC second timestamp")
            report = check_catalog(catalog, raw, checked_at=args.checked_at)
            _write_json(args.output, report)
            if args.github_output:
                _write_github_outputs(args.github_output, report)
            print(json.dumps(report["summary"], sort_keys=True))
            return 2 if args.fail_on_errors and report["summary"]["errors"] else 0
        count = render_issue_descriptors(args.report, args.candidate_results,
                                         args.output_dir, args.run_url)
        print(f"Rendered {count} receiver compatibility issue descriptor(s).")
        return 0
    except WatcherError as error:
        print(f"receiver-update-check: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
