#!/usr/bin/env python3
"""Deterministic, stdlib-only source budget; consumed by the architecture CI suite.

The initial snapshot is sealed to prevent a blind baseline refresh. Intentional
changes append exact, chained adjustments with review metadata. This is a review
aid, not proof of human approval; weakening this checker is itself a policy change.
"""
import argparse
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASELINE = "test/script/repository_budget.json"
SNAPSHOT_SHA256 = "3dbb10c918f988612439d8175beb9549c27e425a29b62d5c492136f18664c7f8"
MAX_LINES = 1500
MAX_BYTES = 128 * 1024
CONTEXT_SHA256 = "5ff3888950b48c0071f0304f66dbc738f3ab814b607db63b7489d96c5e2ced4a"
PRODUCTION = {"src", "core", "api", "html", "cmake", "packaging", "Formula",
              "services", "runtime", "config", "oprs", "contrib", "lib",
              "schemas", "script", "tools", ".github"}
DOCUMENTATION = {".md", ".rst", ".png", ".jpg", ".jpeg", ".gif", ".ico",
                 ".svg", ".woff", ".woff2", ".ttf"}
REVIEW_FIELDS = {"owner", "purpose", "rejected_reuse", "cleanup_plan"}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"),
                                    ensure_ascii=True).encode()).hexdigest()


def source_paths(root):
    # Ignored local builds are not repository contents; force-added artifacts are.
    output = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root, check=True, capture_output=True).stdout
    return sorted({p for p in output.decode("utf-8").split("\0")
                   if p and ((root / p).is_file() or (root / p).is_symlink())})


def production(path):
    item = PurePosixPath(path)
    if item.suffix.lower() in DOCUMENTATION or item.name.startswith("LICENSE"):
        return False
    if item.parts[0] not in PRODUCTION:
        return path in {"CMakeLists.txt", "CMakePresets.json"}
    # Tests co-located with JS/operational code have their own CI consumers.
    if item.parts[0] in {"api", "services", "script", "tools"}:
        if re.search(r"\.test\.(?:js|mjs|cjs|sh)$", path) or item.name.startswith("test_"):
            return False
    return True


def artifact_problem(path):
    item = PurePosixPath(path)
    bad = {"vendor", "vendored", "third_party", "third-party", "generated", "dist",
           "target", "node_modules", "__pycache__", "cmakefiles", ".venv",
           ".pytest_cache", ".mypy_cache", "coverage", "frozen", "backup", "backups"}
    # Provenance/license text is not a vendored implementation.
    notice = item.suffix == ".md" or item.name.startswith("LICENSE")
    if any(p.lower() in bad for p in item.parts) and not notice:
        return "generated/vendor/backup tree"
    if (item.parts[0].startswith("build") or item.parts[0] == "bin") and not notice:
        return "generated build tree"
    if item.suffix.lower() in {".o", ".obj", ".a", ".so", ".dylib", ".dll", ".exe",
                              ".pyc", ".pyo", ".class", ".wasm", ".bak", ".orig"}:
        return "generated binary/backup artifact"
    if item.name in {"CMakeCache.txt", "compile_commands.json", ".DS_Store"}:
        return "generated build/host artifact"
    return None


def dependency_keys(path, data):
    """Inventory dependency identities, permitting ordinary version updates."""
    name = PurePosixPath(path).name
    if name == "package.json":
        document = json.loads(data)
        return [f"{scope}:{name}" for scope in
                ("dependencies", "devDependencies", "optionalDependencies", "peerDependencies")
                for name in document.get(scope, {})]
    if re.fullmatch(r"vcpkg(?:-[\w-]+)?\.json", name):
        document = json.loads(data)
        keys = []
        groups = {"dependencies": document.get("dependencies", [])}
        groups.update({f"feature:{key}": value.get("dependencies", [])
                       for key, value in document.get("features", {}).items()})
        for scope, dependencies in groups.items():
            for dep in dependencies:
                dep = {"name": dep} if isinstance(dep, str) else dep
                keys.append(f"{scope}:{dep['name']}")
                keys.extend(f"{scope}:{dep['name']}:feature:{feature}"
                            for feature in dep.get("features", []))
        return keys
    if (name in {"pyproject.toml", "Cargo.toml", "go.mod", "Pipfile", "setup.py",
                 "setup.cfg", "composer.json", "Gemfile", "pom.xml", "build.gradle"}
            or re.fullmatch(r"requirements(?:[.-][\w-]+)?\.txt", name)):
        raise ValueError(f"{path}: new manifest format requires a reviewed dependency parser")
    return None


def measure(root, paths, exclusions):
    metrics, failures, configs = {}, [], {}
    def add(key, value=1):
        metrics[key] = metrics.get(key, 0) + value
    for path in sorted(paths):
        item = PurePosixPath(path)
        problem = artifact_problem(path)
        if problem:
            failures.append(f"{path}: {problem}")
        target = root / path
        # Never follow a link outside the source tree. Alias payloads count once.
        if target.is_symlink():
            if production(path):
                failures.append(f"{path}: production source must not bypass budgets through a symlink")
            try:
                target.resolve(strict=True).relative_to(root.resolve())
            except (ValueError, OSError):
                failures.append(f"{path}: broken or external source symlink")
            continue
        # Text checkouts may use CRLF on Windows; budgets use canonical LF bytes.
        data = target.read_bytes().replace(b"\r\n", b"\n")
        exclusion = exclusions.get(path)
        if exclusion:
            if exclusion["kind"] == "third_party_minified":
                if hashlib.sha256(data).hexdigest() != exclusion["sha256"]:
                    failures.append(f"{path}: third-party exclusion digest changed")
            elif exclusion["kind"] == "generated_lockfile":
                lock = json.loads(data)
                if item.name != "package-lock.json" or not isinstance(lock.get("lockfileVersion"), int):
                    failures.append(f"{path}: invalid generated lockfile exclusion")
            else:
                failures.append(f"{path}: unknown exclusion kind")
            continue
        if item.name in {"package-lock.json", "yarn.lock", "pnpm-lock.yaml", "Cargo.lock"}:
            failures.append(f"{path}: unreviewed generated lockfile exclusion")
        dependencies = dependency_keys(path, data)
        if dependencies is not None:
            metrics[f"manifest:{path}"] = 1
            for dependency in dependencies:
                metrics[f"dependency:{path}:{dependency}"] = 1
        if path.startswith("src/process/"):
            for parent in item.parents:
                if str(parent).startswith("src/process/"):
                    metrics[f"processing_directory:{parent}"] = 1
        is_config = ((item.suffix in {".yml", ".yaml"} and item.parts[0] != ".github")
                     or (item.parts[0] == "config" and item.suffix == ".json"))
        if is_config and production(path):
            metrics[f"configuration:{path}"] = 1
            # JSON is semantic; YAML preserves indentation and inline values but
            # ignores blank/comment-only lines. No third-party YAML parser needed.
            normalized = (json.dumps(json.loads(data), sort_keys=True) if item.suffix == ".json"
                          else "\n".join(line.rstrip() for line in data.decode().splitlines()
                                         if line.strip() and not line.lstrip().startswith("#")))
            if normalized in configs:
                failures.append(f"{path}: duplicates configuration {configs[normalized]}")
            configs[normalized] = path
        if not production(path):
            continue
        bucket = item.parts[0] if len(item.parts) > 1 else "root"
        add(f"files:{bucket}")
        lines = len(data.splitlines())
        add(f"lines:{bucket}", lines)
        if lines > MAX_LINES:
            metrics[f"oversized_lines:{path}"] = lines
        if len(data) > MAX_BYTES:
            metrics[f"oversized_bytes:{path}"] = len(data)
    return dict(sorted(metrics.items())), failures


def local_references(path, text, paths):
    """Conservative literal dependency index, not a complete language/call graph.

    Computed loads have a separate no-growth counter. Direct foreign modules are
    counted in full; external packages and transitive implementations are not
    modelled. This is a review boundary, never proof of semantic independence.
    """
    refs = re.findall(r'(?:require(?:\.resolve)?|import)\s*\(\s*[\"\']([^\"\']+)', text)
    refs += re.findall(r'\b(?:from|import)\s*[\"\']([^\"\']+)', text)
    refs += re.findall(r'#\s*include\s*[<\"]([^\">]+)[\">]', text)
    refs += re.findall(r'(?:src|href)\s*=\s*[\"\']([^\"\']+)', text)
    refs += re.findall(r'path\.join\(\s*__dirname\s*,\s*[\"\']([^\"\']+)', text)
    if path.endswith('.py'):
        refs += [name.replace('.', '/') + '.py' for name in re.findall(
            r'^\s*(?:from|import)\s+([\w.]+)', text, re.M)]
    if PurePosixPath(path).suffix in {'.cmake', '.sh', '.py', '.yml', '.yaml'} or path.endswith('CMakeLists.txt'):
        refs += re.findall(r'\b(?:src|core|api|html|cmake|script|tools|config|packaging)/[\w./-]+\.[\w]+', text)
    targets = set()
    for ref in refs:
        if re.match(r'(?:\w+://|data:|#)', ref):
            continue
        ref = ref.split('?', 1)[0]
        candidates = [posixpath.join(str(PurePosixPath(path).parent), ref),
                      ref.lstrip('/'), 'src/' + ref, 'html/' + ref.lstrip('/')]
        for candidate in candidates:
            candidate = posixpath.normpath(candidate)
            found = next((p for p in (candidate, candidate + '.js', candidate + '/index.js')
                          if p in paths), None)
            if found:
                targets.add(found)
                break
    dynamic = len(re.findall(r'\b(?:require(?:\.resolve)?|import)\s*\(\s*(?![\s\"\'])[\w$]', text))
    dynamic += len(re.findall(r'\b(?:__import__|import_module)\s*\(', text))
    return targets, dynamic


def measure_context(root, paths, exclusions, policy):
    sources = {p: (root / p).read_bytes().replace(b'\r\n', b'\n').decode('utf-8')
               for p in paths if production(p) and p not in exclusions and not (root / p).is_symlink()}
    owners = {}
    for path in sources:
        owner = next((row['owner'] for row in policy['owners']
                      if any(fnmatch.fnmatchcase(path, pattern) for pattern in row['patterns'])), None)
        if owner is None:
            raise ValueError(f'{path}: no canonical context owner')
        owners[path] = owner
    sizes = {p: {'characters': len(text), 'lines': len(text.encode().splitlines()),
                 'tokens': len(re.findall(r'\w+|[^\w\s]', text))} for p, text in sources.items()}
    metrics, packets = {}, {}
    def record(label, members):
        metrics[f'context_files:{label}'] = len(members)
        for kind in ('characters', 'lines', 'tokens'):
            metrics[f'context_{kind}:{label}'] = sum(sizes[p][kind] for p in members)
    record('production', sources)
    for owner in sorted(set(owners.values())):
        members = sorted(p for p in sources if owners[p] == owner)
        contracts, edges, dynamic = set(), 0, 0
        for path in members:
            dependencies, computed = local_references(path, sources[path], sources)
            foreign = {target for target in dependencies if owners[target] != owner}
            contracts.update(foreign)
            edges += len(foreign)
            dynamic += computed
        record(owner, members)
        record(owner + '/contracts', contracts)
        metrics[f'context_imports:{owner}'] = edges
        metrics[f'context_dynamic:{owner}'] = dynamic
        for target in contracts:
            metrics[f'context_contract:{owner}->{target}'] = 1
        packets[owner] = {'owner_files': members, 'contract_files': sorted(contracts),
                          'characters': sum(sizes[p]['characters'] for p in set(members) | contracts),
                          'lexical_tokens': sum(sizes[p]['tokens'] for p in set(members) | contracts),
                          'lines': sum(sizes[p]['lines'] for p in set(members) | contracts)}
        packets[owner]['oversized_legacy_debt'] = (
            metrics[f'context_characters:{owner}'] > policy['preferred_owner_characters'] or
            packets[owner]['characters'] > policy['preferred_packet_characters'])
    return dict(sorted(metrics.items())), packets


def limits(document, expected_digest=SNAPSHOT_SHA256, context_digest=CONTEXT_SHA256):
    keys = {"schema_version", "snapshot", "adjustments"}
    version = document.get('schema_version')
    if version == 2:
        keys.add('context_snapshot')
    if set(document) != keys or version not in (1, 2):
        raise ValueError("unsupported repository budget schema")
    snapshot = document["snapshot"]
    if digest(snapshot) != expected_digest:
        raise ValueError("initial snapshot changed; append reviewed exact adjustments instead of rebasing")
    ceilings = dict(snapshot["metrics"])
    if version == 2:
        context = document['context_snapshot']
        if digest(context) != context_digest:
            raise ValueError('context ownership snapshot changed; append reviewed adjustments instead of rebasing')
        sealed = context['sealed_adjustments']
        if digest(document['adjustments'][:sealed['count']]) != sealed['sha256']:
            raise ValueError('reviewed adjustment prefix changed; downward ceilings cannot be removed')
        ceilings.update(context['metrics'])
    for key, value in ceilings.items():
        if type(value) is not int or value < 0:
            raise ValueError(f"{key}: invalid budget ceiling")
    for index, adjustment in enumerate(document["adjustments"]):
        grouped = version == 2 and 'changes' in adjustment
        if set(adjustment) != REVIEW_FIELDS | ({'changes'} if grouped else {"metric", "from", "to"}):
            raise ValueError(f"adjustment {index}: exact metric/from/to and review metadata required")
        if any(not isinstance(adjustment[field], str) or len(adjustment[field].strip()) < 12
               for field in REVIEW_FIELDS):
            raise ValueError(f"adjustment {index}: owner/purpose/rejected_reuse/cleanup_plan must be explicit")
        changes = adjustment['changes'] if grouped else {
            adjustment['metric']: [adjustment['from'], adjustment['to']]}
        if not isinstance(changes, dict) or not changes:
            raise ValueError(f'adjustment {index}: nonempty exact changes required')
        for key, pair in changes.items():
            if not isinstance(key, str) or not re.match(
                    r"^(?:files|lines|oversized_lines|oversized_bytes|manifest|dependency|"
                    r"configuration|processing_directory|context_(?:files|characters|lines|tokens|imports|dynamic|contract)):[^*?]+$", key):
                raise ValueError(f"adjustment {index}: exact supported metric required")
            if not isinstance(pair, list) or len(pair) != 2:
                raise ValueError(f'adjustment {index}: exact from/to pair required')
            before, after = pair
            if (type(before) is not int or type(after) is not int or after < 0
                    or before != ceilings.get(key, 0) or before == after):
                raise ValueError(f"adjustment {index}: invalid/non-chained ceiling change")
            ceilings[key] = after
    return ceilings


def check(root=ROOT, document=None, paths=None, expected_digest=SNAPSHOT_SHA256,
          context_digest=CONTEXT_SHA256):
    document = document if document is not None else json.loads((root / BASELINE).read_text())
    ceilings = limits(document, expected_digest, context_digest)
    paths = source_paths(root) if paths is None else paths
    metrics, failures = measure(root, paths,
                                document["snapshot"]["exclusions"])
    if document['schema_version'] == 2:
        context, _ = measure_context(root, paths, document['snapshot']['exclusions'],
                                     document['context_snapshot']['policy'])
        metrics.update(context)
    for key, value in metrics.items():
        if value > ceilings.get(key, 0):
            failures.append(f"{key}: {value} exceeds {ceilings.get(key, 0)}; consolidate or append a reviewed adjustment")
    if document['schema_version'] == 2:
        for key, ceiling in ceilings.items():
            if metrics.get(key, 0) < ceiling:
                failures.append(f'{key}: ratchet {ceiling} down to {metrics.get(key, 0)}; deleted headroom cannot be reused')
    return metrics, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--metrics", action="store_true", help="print measured metrics; never rewrite the baseline")
    parser.add_argument('--review-owner', help='emit one canonical owner plus its bounded direct contract files')
    args = parser.parse_args()
    try:
        metrics, failures = check(args.root)
        if args.review_owner:
            document = json.loads((args.root / BASELINE).read_text())
            _, packets = measure_context(args.root, source_paths(args.root), document['snapshot']['exclusions'],
                                         document['context_snapshot']['policy'])
            if args.review_owner not in packets:
                raise ValueError('unknown context owner; available: ' + ', '.join(packets))
            print(json.dumps(packets[args.review_owner], indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Repository budget: {error}\n")
    if args.metrics:
        print(json.dumps(metrics, indent=2, sort_keys=True))
    if failures:
        parser.exit(1, "Repository budget failed:\n" + "\n".join(failures) + "\n")
    print(f"Repository budget passed ({len(metrics)} measured ceilings).")


if __name__ == "__main__":
    main()
