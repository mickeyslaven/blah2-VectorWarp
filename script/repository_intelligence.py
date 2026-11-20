#!/usr/bin/env python3
"""Source-evidenced repository routing for maintainers and architecture CI.

This inventory never executes repository code/builds or infers semantic coverage.
It reuses the sealed budget's owners; generated packets belong outside the tree.
"""
import argparse
import ast
from collections import Counter, defaultdict
import fnmatch
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex

import check_repository_budget as budget

LEDGER = 'test/script/repository_behaviors.json'
FACETS = ('positive', 'negative', 'reset_failure', 'cross_adapter')
LIMITATIONS = [
    'Lexical references may occur in inactive branches, strings or comments; not call/coverage edges.',
    'CMake declarations are candidates across all branches; no variable, macro or generator evaluation.',
    'Candidate CMake nodes group literal names; optional configured nodes retain build/configuration identity.',
    'Optional artifact bindings are caller attestations checked for content freshness, not authenticated build receipts.',
    'Configured build dependencies are not direct linkage; CTest inventory and CI declarations are not execution coverage.',
    'No dynamic route/module, runtime trace or physical qualification evidence.',
    'Authored behavior bindings describe required checks; no test was run by this extractor.',
    'Assertion anchors are unique lexical excerpts, not parsed assertions or proof that a command selects them.',
    'Variant cells are authored seed obligations, not an exhaustive platform/receiver/backend cross-product or qualification.',
    'Binary and sealed third-party/lockfile payloads are hashed but not parsed.',
]
REFERENCE = re.compile(
    r'''\b(?:require(?:\.resolve)?|import)\s*\(\s*(?:'(?P<js_single>[^'\\]*)'|"(?P<js_double>[^"\\]*)")\s*(?=[),])'''
    r'''|\b(?:from|import)\s*["'](?P<esm>[^"']+)["']'''
    r'|#\s*include\s*[<"](?P<cpp>[^">]+)[">]'
    r'''|(?:src|href)\s*=\s*["'](?P<asset>[^"']+)["']'''
    r'|(?P<path>\b(?:src|core|api|html|cmake|script|tools|config|packaging|test)/[\w./-]+\.[\w]+)')
MODULE_CALL = re.compile(r'\b(?:require(?:\.resolve)?|import)\s*\(')
MODULE_LITERAL = re.compile(r'''\s*(?:'[^'\\]*'|"[^"\\]*")\s*(?=[),])''')


def line_at(text, offset):
    return text.count('\n', 0, offset) + 1


def javascript_array_items(source):
    """Return top-level array slots and offsets, or None for malformed/non-arrays."""
    source = source.strip()
    if len(source) < 2 or source[0] != '[' or source[-1] != ']':
        return None
    if not source[1:-1].strip():
        return []
    items, start, depth, quote, escaped = [], 1, 0, None, False
    index = 1
    while index < len(source) - 1:
        char = source[index]
        if quote:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == quote:
                quote = None
            index += 1
            continue
        if source.startswith('//', index):
            newline = source.find('\n', index + 2)
            index = len(source) - 1 if newline < 0 else newline + 1
            continue
        if source.startswith('/*', index):
            closing = source.find('*/', index + 2)
            if closing < 0:
                return None
            index = closing + 2
            continue
        if char in "'\"`":
            quote = char
        elif char in '[{(':
            depth += 1
        elif char in ']})':
            depth -= 1
            if depth < 0:
                return None
        elif char == ',' and depth == 0:
            raw = source[start:index]
            items.append((raw.strip(), start + len(raw) - len(raw.lstrip())))
            start = index + 1
        index += 1
    if quote or depth:
        return None
    raw = source[start:-1]
    if raw.strip():
        items.append((raw.strip(), start + len(raw) - len(raw.lstrip())))
    return items


def javascript_routes(text):
    """Yield literal Express routes and unresolved route-variable registrations."""
    loop = re.compile(r'''for\s*\(\s*const\s+(?P<binding>\[\s*route\b[^\]]*\]|route)\s+of\s+(?P<values>\[(?:(?!\bfor\s*\()[\s\S])*?\])\s*\)\s*\{?\s*(?:app|router)\.(?P<method>get|post|put|patch|delete|head|options)\s*\(\s*route\s*,''')
    resolved = set()
    for match in loop.finditer(text):
        resolved.add(match.start('method'))
        values = match['values']
        items = javascript_array_items(values)
        if items is None:
            yield match['method'].upper(), None, line_at(text, match.start('values')), values
            continue
        for item, offset in items:
            candidate, candidate_offset = item, offset
            if match['binding'].startswith('['):
                nested = javascript_array_items(item)
                if nested is None or not nested:
                    yield match['method'].upper(), None, line_at(text, match.start('values') + offset), item or '<elided-route>'
                    continue
                candidate, inner_offset = nested[0]
                candidate_offset += inner_offset
            literal = re.fullmatch(r'''(["'])(/[^"'\\]*)\1''', candidate)
            if literal:
                yield match['method'].upper(), literal[2], line_at(text, match.start('values') + candidate_offset), None
            else:
                yield match['method'].upper(), None, line_at(text, match.start('values') + candidate_offset), candidate or '<elided-route>'
    calls = re.compile(r'''\b(?:app|router)\.(?P<method>get|post|put|patch|delete|head|options)\s*\(''')
    for match in calls.finditer(text):
        if match.start('method') in resolved:
            continue
        start, index, depth, quote, escaped = match.end(), match.end(), 0, None, False
        while index < len(text):
            char = text[index]
            if quote:
                if escaped:
                    escaped = False
                elif char == '\\':
                    escaped = True
                elif char == quote:
                    quote = None
            elif char in "'\"`":
                quote = char
            elif char in '[{(':
                depth += 1
            elif char in ']})':
                if char == ')' and depth == 0:
                    break
                depth -= 1
            elif char == ',' and depth == 0:
                break
            index += 1
        argument = text[start:index].strip()
        offset = start + len(text[start:index]) - len(text[start:index].lstrip())
        literal = re.fullmatch(r'''(["'])(/[^"'\\]*)\1''', argument)
        if literal:
            yield match['method'].upper(), literal[2], line_at(text, offset), None
        else:
            yield match['method'].upper(), None, line_at(text, offset), argument or '<missing-route-argument>'


def resolve(path, reference, paths):
    """Resolve a literal only; never guess by basename or inspect external files."""
    if re.match(r'(?:\w+://|data:|#)', reference) or '$' in reference:
        return None
    reference = reference.split('?', 1)[0].split('#', 1)[0]
    candidates = [posixpath.join(str(PurePosixPath(path).parent), reference),
                  reference.lstrip('/'), 'src/' + reference, 'html/' + reference.lstrip('/')]
    for candidate in candidates:
        candidate = posixpath.normpath(candidate)
        for name in (candidate, candidate + '.js', candidate + '.mjs',
                     candidate + '/index.js', candidate + '.py', candidate + '/__init__.py'):
            if name in paths:
                return name
    return None


def source_target(root, path):
    """Validate ancestors too: tracked children can survive an ignored symlink swap."""
    target = root / path
    try:
        target.resolve(strict=True).relative_to(root)
    except (ValueError, OSError, RuntimeError) as error:
        raise ValueError(f'{path}: broken/external symlink or source escape') from error
    if budget.production(path) and any((root / parent).is_symlink()
                                       for parent in (Path(path), *Path(path).parents)):
        raise ValueError(f'{path}: production symlink cannot bypass owner accounting')
    return target


def module_calls(text):
    """Classify whole first arguments; computed strings must not acquire exact edges."""
    for match in MODULE_CALL.finditer(text):
        end, depth, quote, escaped = match.end(), 1, None, False
        while end < len(text) and depth:
            char = text[end]
            if escaped:
                escaped = False
            elif quote:
                if char == '\\':
                    escaped = True
                elif char == quote:
                    quote = None
            elif text.startswith(('//', '/*'), end):
                marker = '\n' if text.startswith('//', end) else '*/'
                stop = text.find(marker, end + 2)
                end = len(text) if stop < 0 else stop + len(marker)
                continue
            elif char in "\"'`":
                quote = char
            else:
                depth += (char == '(') - (char == ')')
            end += 1
        yield match.start(), end, MODULE_LITERAL.match(text, match.end()) is not None


def cmake_commands(text):
    """Balanced literal commands, including multiline arguments; no CMake evaluation."""
    # Consume quoted/escaped tokens before comments, retaining original offsets.
    text = re.sub(r'"(?:\\.|[^"\\])*"|\\.|#\[(=*)\[.*?\]\1\]|#[^\n]*',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0])
                  if m[0].startswith('#') else m[0], text, flags=re.S)
    position = 0
    while match := re.search(r'\b([A-Za-z_]\w*)\s*\(', text[position:]):
        start = position + match.start()
        opening = position + match.end()
        end, depth, quoted, escaped = opening, 1, False, False
        while end < len(text) and depth:
            char = text[end]
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                quoted = not quoted
            elif not quoted:
                depth += (char == '(') - (char == ')')
            end += 1
        if depth:
            raise ValueError(f'unclosed CMake command at line {line_at(text, start)}')
        yield match[1].lower(), shlex.split(text[opening:end - 1], comments=False), line_at(text, start)
        position = end


def configured_evidence(root, manifest, bundle, node, edge, gap):
    """Ingest pinned inventories only; never configure, execute tests, or trust CI results."""
    def require(condition, message):
        if not condition:
            raise ValueError('configured evidence: ' + message)

    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON key ' + key)
            result[key] = value
        return result

    def rows(data, key):
        value = data.get(key, [])
        require(isinstance(value, list) and all(isinstance(row, dict) for row in value),
                'invalid ' + key + ' inventory')
        return value

    def version(data, major):
        value = data.get('version', {})
        return (isinstance(value, dict) and type(value.get('major')) is int and value['major'] == major
                and type(value.get('minor', 0)) is int and value.get('minor', 0) >= 0)

    bundle = Path(bundle).absolute()
    require(not bundle.resolve().is_relative_to(root), 'bundle must remain outside the repository')
    require(not any(path.is_symlink() for path in (bundle, *bundle.parents)), 'manifest symlink')
    raw_manifest = bundle.read_bytes()
    receipt = json.loads(raw_manifest, object_pairs_hook=unique_object)
    require(isinstance(receipt, dict) and type(receipt.get('schema_version')) is int and
            receipt['schema_version'] == 1, 'unsupported manifest schema')
    require(receipt.get('source_digest') == budget.digest(manifest), 'stale source digest')
    require(receipt.get('source_root') == str(root), 'source root mismatch')
    require(set(receipt) <= {'schema_version', 'source_digest', 'source_root', 'artifacts', 'cmake', 'ci_jobs'},
            'unknown manifest fields')
    hashes = receipt.get('artifacts')
    require(isinstance(hashes, dict) and hashes, 'missing artifact hashes')
    payloads, consumed = {}, set()

    def artifact_path(name):
        require(isinstance(name, str) and name and '\\' not in name and
                not PurePosixPath(name).is_absolute() and
                all(part not in {'', '.', '..'} for part in name.split('/')), 'unsafe artifact path')
        path = bundle.parent / name
        require(path.resolve().is_relative_to(bundle.parent.resolve()) and
                not path.resolve().is_relative_to(root) and
                not any(part.is_symlink() for part in (path, *path.parents)), 'artifact symlink or escape')
        return path

    for name, expected in hashes.items():
        require(isinstance(expected, str) and re.fullmatch('[0-9a-f]{64}', expected), 'invalid artifact hash')
        raw = artifact_path(name).read_bytes()
        require(hashlib.sha256(raw).hexdigest() == expected, 'stale artifact: ' + name)
        payloads[name] = raw

    def read(name):
        require(isinstance(name, str) and name in payloads, 'unsealed artifact reference: ' + str(name))
        consumed.add(name)
        value = json.loads(payloads[name], object_pairs_hook=unique_object)
        require(isinstance(value, dict), 'artifact must be a JSON object: ' + name)
        return value

    def evidence(source, target, kind, name, pointer, basis='configured'):
        edge(source, target, kind, str(artifact_path(name)), 1, basis, pointer)

    def source_path(value):
        require(isinstance(value, str) and value, 'invalid source path')
        path = posixpath.normpath(posixpath.join(str(root), value))
        if path != posixpath.join(str(root), value):
            return None  # Lexical '..' cancellation is unsafe across symlink components.
        return posixpath.relpath(path, str(root)) if path.startswith(str(root) + '/') else None

    def backtrace(identity, data, index, name, pointer):
        if index is None:
            gap(str(artifact_path(name)), 1, 'missing_configured_backtrace', identity)
            return
        trace = data.get('backtraceGraph', {})
        trace_rows, files = rows(trace, 'nodes'), trace.get('files', [])
        require(isinstance(files, list) and all(isinstance(path, str) for path in files), 'invalid backtrace files')
        require(type(index) is int and 0 <= index < len(trace_rows), 'invalid backtrace index')
        row = trace_rows[index]
        require(isinstance(row, dict) and type(row.get('file')) is int and
                0 <= row['file'] < len(files), 'invalid backtrace file')
        path, line = source_path(files[row['file']]), row.get('line')
        if path not in manifest or line is None:
            gap(str(artifact_path(name)), 1, 'unresolved_configured_backtrace', identity)
            return
        require(type(line) is int and 1 <= line <= len(source_target(root, path).read_bytes().splitlines()),
                'invalid backtrace line')
        evidence(identity, 'file:' + path, 'configured_backtrace', name, pointer)

    cmake = receipt.get('cmake')
    if cmake is not None:
        require(isinstance(cmake, dict) and set(cmake) == {'codemodel', 'ctest', 'configuration', 'build_root'},
                'invalid CMake inventory selection')
        config, build = cmake['configuration'], cmake['build_root']
        require(isinstance(config, str) and isinstance(build, str) and
                posixpath.isabs(build) and posixpath.normpath(build) == build and
                not Path(build).is_relative_to(root) and not Path(build).resolve().is_relative_to(root),
                'invalid configuration/build root')
        model_name = cmake['codemodel']
        model = read(model_name)
        require(model.get('kind') == 'codemodel' and version(model, 2),
                'unsupported CMake codemodel')
        require(model.get('paths') == {'source': str(root), 'build': build}, 'CMake source/build root mismatch')
        configurations = rows(model, 'configurations')
        selected = [(i, row) for i, row in enumerate(configurations) if row.get('name') == config]
        require(len(selected) == 1, 'configuration missing or ambiguous')
        config_index, selected = selected[0]
        require(isinstance(selected.get('targets'), list), 'missing configured target inventory')
        configuration = node('cmake_configuration:' + json.dumps([build, config]), 'cmake_configuration',
                             build_root=build, configuration=config)
        targets, executables = {}, defaultdict(list)
        for index, entry in enumerate(rows(selected, 'targets')):
            target_id = entry.get('id')
            require(isinstance(target_id, str) and target_id and target_id not in targets, 'duplicate/invalid target id')
            name = posixpath.join(posixpath.dirname(model_name), entry['jsonFile'])
            target = read(name)
            require(target.get('id') == target_id and target.get('name') == entry.get('name') and
                    isinstance(target.get('name'), str) and target['name'], 'target identity mismatch')
            require(isinstance(target.get('type'), str) and target['type'] in {
                'EXECUTABLE', 'STATIC_LIBRARY', 'SHARED_LIBRARY', 'MODULE_LIBRARY', 'OBJECT_LIBRARY',
                'INTERFACE_LIBRARY', 'UTILITY', 'UNKNOWN_LIBRARY'}, 'invalid target type')
            sources, dependencies, artifacts = (rows(target, key) for key in ('sources', 'dependencies', 'artifacts'))
            require(all(isinstance(item.get('id'), str) and item['id'] for item in dependencies), 'invalid dependency id')
            require(all(isinstance(item.get('path'), str) and item['path'] for item in artifacts), 'invalid target artifact path')
            identity = node('cmake_configured:' + json.dumps([build, config, target_id]),
                            'cmake_target_configured', name=target['name'], target_id=target_id,
                            build_root=build, configuration=config, target_type=target.get('type'))
            targets[target_id] = (identity, name, target)
            evidence(configuration, identity, 'configured_target_inventory', model_name,
                     f'/configurations/{config_index}/targets/{index}')
            for source_index, source in enumerate(sources):
                require(type(source.get('isGenerated', False)) is bool, 'invalid generated source flag')
                path = source_path(source.get('path'))
                if source.get('isGenerated') or path not in manifest:
                    gap(str(artifact_path(name)), 1, 'generated_or_external_build_input', source['path'])
                else:
                    evidence(identity, 'file:' + path, 'configured_build_input', name, f'/sources/{source_index}')
            if target.get('type') == 'EXECUTABLE':
                for artifact in artifacts:
                    executables[posixpath.join(build, artifact['path'])].append(identity)
        for identity, name, target in targets.values():
            for index, dependency in enumerate(target.get('dependencies', [])):
                if dependency.get('id') not in targets:
                    gap(str(artifact_path(name)), 1, 'unresolved_configured_dependency', str(dependency.get('id')))
                else:
                    evidence(identity, targets[dependency['id']][0], 'configured_build_dependency', name,
                             f'/dependencies/{index}')
        ctest_name = cmake['ctest']
        ctest = read(ctest_name)
        require(ctest.get('kind') == 'ctestInfo' and version(ctest, 1),
                'unsupported CTest inventory')
        require(isinstance(ctest.get('tests'), list), 'missing CTest test inventory')
        seen = set()
        for index, test in enumerate(rows(ctest, 'tests')):
            name, command = test.get('name'), test.get('command', [])
            require(isinstance(name, str) and name and name not in seen, 'duplicate/invalid CTest name')
            require(test.get('config', config) == config, 'CTest configuration mismatch')
            require(isinstance(command, list) and all(isinstance(arg, str) for arg in command), 'invalid CTest command')
            seen.add(name)
            identity = node('ctest_configured:' + json.dumps([build, config, name]), 'ctest_configured',
                            name=name, configuration=config, build_root=build, command=command, execution='pending')
            evidence(configuration, identity, 'configured_test_inventory', ctest_name, f'/tests/{index}')
            backtrace(identity, ctest, test.get('backtrace'), ctest_name, f'/tests/{index}/backtrace')
            matches = executables.get(command[0], []) if command else []
            if len(matches) == 1:
                evidence(identity, matches[0], 'configured_test_command', ctest_name, f'/tests/{index}/command/0')
            else:
                gap(str(artifact_path(ctest_name)), 1, 'unresolved_configured_test_command', name)
    ci_name = receipt.get('ci_jobs')
    if ci_name is not None:
        ci = read(ci_name)
        require(type(ci.get('schema_version')) is int and ci['schema_version'] == 1 and
                isinstance(ci.get('jobs'), list), 'unsupported CI inventory')
        seen = set()
        for index, job in enumerate(rows(ci, 'jobs')):
            workflow, name, line = job.get('workflow'), job.get('job'), job.get('line')
            require(isinstance(workflow, str) and workflow in manifest and
                    workflow.startswith('.github/workflows/') and workflow.endswith(('.yml', '.yaml')),
                    'missing CI workflow')
            require(isinstance(name, str) and re.fullmatch('[A-Za-z_][A-Za-z0-9_-]*', name) and
                    (workflow, name) not in seen, 'duplicate/invalid CI job')
            text = source_target(root, workflow).read_text()
            lines = text.splitlines()
            require(type(line) is int and 1 <= line <= len(lines) and
                    lines[line - 1].rstrip() == '  ' + name + ':', 'CI job source line mismatch')
            # Refuse unsupported keys before masking: erasing one can hide a block boundary or duplicate.
            def mask_scalar(match):
                require(not (match[0][0] in "\"'" and re.match(r'\s*:', text[match.end():])),
                        'unsupported quoted CI mapping key')
                return ''.join('\n' if char == '\n' else ' ' for char in match[0])
            masked = re.sub(r'''"(?:\\.|[^"\\])*"|'(?:''|[^'])*'|#[^\n]*''',
                            mask_scalar, text)
            require('"' not in masked and "'" not in masked, 'unsupported CI scalar quoting')
            declarations, inside, blocks = {}, False, 0
            for number, candidate in enumerate(masked.splitlines(), 1):
                if candidate.strip() and not candidate[0].isspace():
                    inside = candidate.rstrip() == 'jobs:'
                    blocks += inside
                match = re.fullmatch(r'  ([A-Za-z_][A-Za-z0-9_-]*):\s*', candidate) if inside else None
                require(not (inside and candidate.strip() and len(candidate) - len(candidate.lstrip()) == 2
                             and not match), 'unsupported CI job declaration')
                if match:
                    require(match[1] not in declarations, 'duplicate CI source job')
                    declarations[match[1]] = number
            require(blocks == 1 and declarations.get(name) == line, 'CI job is outside a literal jobs block')
            seen.add((workflow, name))
            identity = node('ci_job:' + workflow + ':' + name, 'ci_job_candidate',
                            name=name, workflow=workflow, execution='pending')
            evidence('file:' + workflow, identity, 'reports_ci_job', ci_name, f'/jobs/{index}', 'reported_ci_inventory')
            edge('file:' + workflow, identity, 'declares', workflow, line, 'candidate')
    require(cmake is not None or ci_name is not None, 'no inventories selected')
    require(consumed == set(payloads), 'unconsumed sealed artifacts')
    require(bundle.read_bytes() == raw_manifest, 'manifest changed during extraction')
    for name, expected in hashes.items():
        require(hashlib.sha256(artifact_path(name).read_bytes()).hexdigest() == expected,
                'artifact changed during extraction: ' + name)
    return dict(manifest_sha256=hashlib.sha256(raw_manifest).hexdigest(), artifacts=hashes,
                binding='caller-attested exact-source inventory; not execution or authenticated provenance')


def ledger_object(pairs):
    """Do not let duplicate JSON keys erase required assertion or variant cells."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('behavior ledger: duplicate JSON key ' + key)
        result[key] = value
    return result


def validate_behaviors(ledger, texts):
    """Validate authored schema v2; pending cells never imply execution coverage."""
    def ensure(condition, message):
        if not condition:
            raise ValueError('behavior ledger: ' + message)

    def strings(values):
        return (isinstance(values, list) and bool(values) and
                all(isinstance(value, str) and value.strip() for value in values) and
                len(values) == len(set(values)))

    ensure(isinstance(ledger, dict) and type(ledger.get('schema_version')) is int and
            ledger['schema_version'] == 2, 'unsupported schema')
    ensure(set(ledger) <= {'schema_version', 'purpose', 'behaviors'} and
            isinstance(ledger.get('behaviors'), list), 'invalid ledger fields')
    seen_behaviors, seen_assertions = set(), set()
    for behavior in ledger['behaviors']:
        ensure(isinstance(behavior, dict) and set(behavior) <= {
            'id', 'contract', 'owners', 'required_inputs', 'checks', 'pending',
            'variants', 'assertions', 'variant_cells'}, 'invalid behavior fields')
        identity = behavior.get('id')
        ensure(isinstance(identity, str) and re.fullmatch(r'[a-z][a-z0-9-]*', identity),
                'missing or invalid behavior identity')
        ensure(identity not in seen_behaviors, 'duplicate behavior identity: ' + identity)
        seen_behaviors.add(identity)
        ensure(isinstance(behavior.get('contract'), str) and behavior['contract'].strip() and
                strings(behavior.get('owners')), identity + ': missing contract or owners')
        variants, rows = behavior.get('variants'), behavior.get('assertions')
        ensure(strings(variants), identity + ': missing or duplicate required variants')
        ensure(all(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', variant) and
                    variant != 'default' for variant in variants), identity + ': variants must be explicit literals')
        ensure(isinstance(rows, list) and rows, identity + ': missing assertions')
        assertions = {}
        for assertion in rows:
            ensure(isinstance(assertion, dict) and set(assertion) == {
                'id', 'facet', 'outcome', 'file', 'anchor'}, identity + ': invalid assertion fields')
            name = assertion['id']
            ensure(isinstance(name, str) and re.fullmatch(r'[a-z][a-z0-9.-]*', name),
                    identity + ': missing or invalid assertion identity')
            ensure(name not in seen_assertions, 'duplicate assertion identity: ' + name)
            seen_assertions.add(name)
            ensure(assertion['facet'] in FACETS and isinstance(assertion['outcome'], str) and
                    assertion['outcome'].strip(), name + ': invalid assertion facet or outcome')
            path, anchor = assertion['file'], assertion['anchor']
            ensure(isinstance(path, str) and path in texts, name + ': missing assertion source')
            ensure(isinstance(anchor, str) and anchor.strip() and texts[path].count(anchor) == 1,
                    name + ': missing or ambiguous assertion anchor')
            assertions[name] = assertion
        checks = behavior.get('checks')
        ensure(isinstance(checks, dict) and set(checks) <= set(FACETS), identity + ': invalid checks')
        bound = set()

        def references(names, facet):
            ensure(strings(names), identity + ': missing or duplicate assertion references')
            ensure(all(name in assertions and assertions[name]['facet'] == facet for name in names),
                    identity + ': unknown or wrong-facet assertion reference')

        for facet, commands in checks.items():
            ensure(isinstance(commands, list), identity + ': invalid command list')
            for check in commands:
                ensure(isinstance(check, dict) and set(check) == {'command', 'files', 'assertions'},
                        identity + ': command must name assertions and files')
                ensure(isinstance(check['command'], list) and check['command'] and
                        all(isinstance(arg, str) and arg.strip() for arg in check['command']),
                        identity + ': behavior command must be a nonempty argv list')
                ensure(strings(check['files']) and all(path in texts for path in check['files']),
                        identity + ': missing test or invalid test files')
                references(check['assertions'], facet)
                ensure(all(assertions[name]['file'] in check['files'] for name in check['assertions']),
                        identity + ': command omits assertion source')
                bound.update(check['assertions'])
        ensure(bound == set(assertions), identity + ': assertion has no command binding')
        cells = behavior.get('variant_cells')
        ensure(isinstance(cells, dict) and set(cells) == set(variants),
                identity + ': incomplete or extra required variant mappings')
        for variant, facets in cells.items():
            ensure(isinstance(facets, dict) and set(facets) == set(FACETS),
                    identity + ':' + variant + ': incomplete or extra facet cells')
            for facet, cell in facets.items():
                if isinstance(cell, dict):
                    ensure(set(cell) == {'pending'} and isinstance(cell['pending'], str) and
                            cell['pending'].strip(), identity + ': invalid pending cell')
                else:
                    references(cell, facet)


def extract(root, paths=None, document=None, ledger=None, configured=None):
    root = root.resolve()
    discover = paths is None
    paths = sorted(set(budget.source_paths(root) if paths is None else paths))
    for path in paths:
        source_target(root, path)
    document = document if document is not None else json.loads(source_target(root, budget.BASELINE).read_text())
    ledger = ledger if ledger is not None else json.loads(source_target(root, LEDGER).read_text(),
                                                        object_pairs_hook=ledger_object)
    policy = document['context_snapshot']['policy']
    exclusions = document['snapshot']['exclusions']
    nodes, edges, gaps, texts, manifest = {}, set(), [], {}, {}
    nodes_paths = set(paths)
    policy_text = source_target(root, budget.BASELINE).read_text() if (root / budget.BASELINE).exists() else ''
    ledger_text = source_target(root, LEDGER).read_text() if (root / LEDGER).exists() else ''

    def json_line(text, key, value):
        match = re.search(r'"' + key + r'"\s*:\s*' + re.escape(json.dumps(value)), text)
        return line_at(text, match.start()) if match else 1

    def node(identity, kind, **data):
        if identity not in nodes:
            nodes[identity] = dict(id=identity, kind=kind, **data)
        return identity

    def edge(source, target, kind, path, line, basis='literal', pointer=''):
        edges.add((source, target, kind, path, line, basis, pointer))

    def gap(path, line, reason, value):
        gaps.append(dict(path=path, line=line, reason=reason, value=value))

    for path in paths:
        target = source_target(root, path)
        if target.is_symlink():
            try:
                destination = target.resolve(strict=True).relative_to(root).as_posix()
            except (ValueError, OSError) as error:
                raise ValueError(f'{path}: broken/external symlink') from error
            manifest[path] = 'symlink:' + target.readlink().as_posix()
            node('file:' + path, 'file', path=path, alias=destination, characters=0, lines=0)
            continue
        raw = target.read_bytes()
        manifest[path] = hashlib.sha256(raw).hexdigest()
        authored = budget.production(path) and path not in exclusions
        owner = next((row['owner'] for row in policy['owners']
                      if any(fnmatch.fnmatchcase(path, pattern) for pattern in row['patterns'])), None)
        if authored and owner is None:
            raise ValueError(f'{path}: no canonical production owner')
        try:
            text = raw.decode('utf-8')
        except UnicodeDecodeError:
            text = ''
        identity = node('file:' + path, 'file', path=path, sha256=manifest[path],
                        production=authored, owner=owner if authored else None,
                        lines=len(raw.splitlines()), characters=len(text),
                        lexical_tokens=len(re.findall(r'\w+|[^\w\s]', text)) if path not in exclusions else 0)
        if authored:
            edge(identity, node('owner:' + owner, 'owner', name=owner), 'owned_by',
                 budget.BASELINE, json_line(policy_text, 'owner', owner), 'policy')
        if text and path not in exclusions:
            texts[path] = text
    for path, text in texts.items():
        source = 'file:' + path
        calls = list(module_calls(text)) if path.endswith(('.js', '.mjs', '.cjs', '.html')) else []
        for match in REFERENCE.finditer(text):
            if any(start <= match.start() < end and not literal for start, end, literal in calls):
                continue
            reference = next(value for value in match.groupdict().values() if value is not None)
            target = resolve(path, reference, nodes_paths)
            if target:
                edge(source, 'file:' + target, 'references', path, line_at(text, match.start()))
            elif match.lastgroup != 'path':
                gap(path, line_at(text, match.start()), 'unresolved_or_external_reference', reference)
        for start, end, literal in calls:
            if not literal:
                gap(path, line_at(text, start), 'computed_module', text[start:min(end, start + 80)])
        dynamic = r'\b(?:__import__|import_module)\s*\(' if path.endswith('.py') else r'(?!)'
        for match in re.finditer(dynamic, text):
            gap(path, line_at(text, match.start()), 'computed_module', match[0])
        if path.endswith('.py'):
            try:
                tree = ast.parse(text)
            except SyntaxError as error:
                gap(path, error.lineno, 'python_parse_error', error.msg)
            else:
                for item in ast.walk(tree):
                    if isinstance(item, (ast.Import, ast.ImportFrom)):
                        names = [entry.name for entry in item.names] if isinstance(item, ast.Import) else [item.module or '']
                        for name in names:
                            reference = name.replace('.', '/')
                            if isinstance(item, ast.ImportFrom) and item.level:
                                reference = '../' * (item.level - 1) + reference
                            target = resolve(path, reference, nodes_paths)
                            if target:
                                edge(source, 'file:' + target, 'references', path, item.lineno)
                            else:
                                gap(path, item.lineno, 'unresolved_or_external_python', name)
        if path.endswith('.cmake') or PurePosixPath(path).name == 'CMakeLists.txt':
            for command, args, line in cmake_commands(text):
                if command not in {'add_library', 'add_executable', 'target_sources', 'target_link_libraries', 'add_test'} or not args:
                    continue
                name = args[1] if command == 'add_test' and args[0] == 'NAME' else args[0]
                if '$' in name:
                    gap(path, line, 'computed_cmake_target', name)
                    continue
                kind = 'ctest_candidate' if command == 'add_test' else 'cmake_target_candidate'
                target_id = node(kind + ':' + name, kind, name=name)
                edge(source, target_id, 'declares' if command.startswith('add_') else 'configures', path, line, 'candidate')
                if command == 'add_test' and 'COMMAND' in args and args.index('COMMAND') + 1 < len(args):
                    executable = args[args.index('COMMAND') + 1]
                    edge(target_id, node('cmake_link:' + executable, 'cmake_link', name=executable),
                         'runs_target_candidate', path, line, 'candidate')
                if 'ALIAS' in args and args.index('ALIAS') + 1 < len(args):
                    original = args[args.index('ALIAS') + 1]
                    edge(target_id, node('cmake_link:' + original, 'cmake_link', name=original),
                         'aliases_candidate', path, line, 'candidate')
                for arg in args[1:]:
                    expanded = arg.replace('${PROJECT_ROOT}/', '').replace('${CMAKE_SOURCE_DIR}/', '')
                    target = resolve(path, expanded, nodes_paths)
                    if target:
                        edge(target_id, 'file:' + target, 'test_input_candidate' if command == 'add_test' else 'build_input_candidate', path, line, 'candidate')
                    elif command == 'target_link_libraries' and arg not in {'PUBLIC', 'PRIVATE', 'INTERFACE', 'debug', 'optimized', 'general'}:
                        link = node('cmake_link:' + arg, 'cmake_link', name=arg)
                        edge(target_id, link, 'links_candidate', path, line, 'candidate')
                    elif '$' in expanded:
                        gap(path, line, 'computed_cmake_argument', arg)
        if path.endswith(('.js', '.mjs', '.cjs')):
            for method, value, line, unresolved in javascript_routes(text):
                if unresolved:
                    gap(path, line, 'computed_route', unresolved)
                    continue
                route = node('route:' + method + ' ' + value, 'route_candidate', method=method, route=value)
                edge(source, route, 'declares', path, line, 'candidate')
        if PurePosixPath(path).name == 'package.json':
            package = json.loads(text)
            for name, command in package.get('scripts', {}).items():
                line = line_at(text, text.find(json.dumps(name)))
                runner = node('npm:' + path + ':' + name, 'runner', command=command)
                edge(source, runner, 'declares', path, line)
                for literal in re.findall(r'[\w./-]+\.(?:js|mjs|cjs|py|sh)\b', command):
                    target = resolve(path, literal, nodes_paths)
                    if target:
                        edge(runner, 'file:' + target, 'runs_candidate', path, line, 'candidate')
        if path == 'api/config-rules.js':
            for match in re.finditer(r'''(?m)^\s*["']([\w]+(?:\.[\w]+)+)["']\s*:''', text):
                field = node('setting:' + match[1], 'setting', name=match[1])
                edge(source, field, 'declares', path, line_at(text, match.start()))
    # Link names remain qualified candidates; no claim that CMake selected them.
    for identity, item in list(nodes.items()):
        target = 'cmake_target_candidate:' + item.get('name', '')
        if item['kind'] == 'cmake_link' and target in nodes:
            for source, dest, kind, path, line, basis, pointer in sorted(edges):
                if dest == identity:
                    edge(identity, target, 'names_candidate', path, line, basis)
    fields = {item['name'] for item in nodes.values() if item['kind'] == 'setting'}
    for path, text in texts.items():
        for match in re.finditer(r'''["']([\w]+(?:\.[\w]+)+)["']''', text):
            if match[1] in fields:
                edge('file:' + path, 'setting:' + match[1], 'mentions_setting', path, line_at(text, match.start()))
    validate_behaviors(ledger, texts)
    for behavior in ledger['behaviors']:
        ledger_line = json_line(ledger_text, 'id', behavior['id'])
        identity = node('behavior:' + behavior['id'], 'behavior',
                        **{key: value for key, value in behavior.items() if key != 'id'}, execution='pending')
        for owner in behavior['owners']:
            if 'owner:' + owner not in nodes:
                raise ValueError(f"{behavior['id']}: unknown owner {owner}")
            edge(identity, 'owner:' + owner, 'requires_owner', LEDGER, ledger_line, 'authored')
        for assertion in behavior['assertions']:
            assertion_id = node('assertion:' + assertion['id'], 'behavior_assertion',
                                **{key: value for key, value in assertion.items() if key != 'id'},
                                execution='pending')
            edge(identity, assertion_id, 'requires_assertion', LEDGER,
                 json_line(ledger_text, 'id', assertion['id']), 'authored')
            edge(assertion_id, 'file:' + assertion['file'], 'authored_assertion_anchor',
                 assertion['file'], line_at(texts[assertion['file']],
                 texts[assertion['file']].index(assertion['anchor'])), 'authored')
        for variant, facets in behavior['variant_cells'].items():
            for facet, cell in facets.items():
                if isinstance(cell, dict):
                    gap(LEDGER, ledger_line, 'unbound_behavior_variant_cell',
                        behavior['id'] + ':' + variant + ':' + facet + ': ' + cell['pending'])
        for facet in FACETS:
            if not behavior['checks'].get(facet):
                gap(LEDGER, ledger_line, 'missing_behavior_facet', behavior['id'] + ':' + facet)
            for check in behavior['checks'].get(facet, []):
                for path in check['files']:
                    edge(identity, 'file:' + path, 'requires_' + facet, LEDGER, ledger_line, 'authored')
    try:
        inventories = configured_evidence(root, manifest, configured, node, edge, gap) if configured else None
    except (KeyError, TypeError, AttributeError, IndexError) as error:
        raise ValueError('configured evidence: malformed inventory structure') from error
    # Refuse a mixed snapshot while other bounded workstreams edit this checkout.
    if discover and budget.source_paths(root) != paths:
        raise ValueError('source inventory changed during extraction; retry after the writer finishes')
    for path, expected in manifest.items():
        target = source_target(root, path)
        current = ('symlink:' + target.readlink().as_posix() if target.is_symlink()
                   else hashlib.sha256(target.read_bytes()).hexdigest())
        if current != expected:
            raise ValueError(f'{path}: source changed during extraction; retry after the writer finishes')
    graph = dict(schema_version=1, source_digest=budget.digest(manifest), files=manifest,
                 implementation_digest=budget.digest({p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (Path(__file__), Path(budget.__file__))}),
                 extraction_digest=budget.digest(dict(policy=policy, ledger=ledger)),
                 limitations=LIMITATIONS, nodes=sorted(nodes.values(), key=lambda row: row['id']),
                 edges=[dict(source=a, target=b, kind=c, evidence=dict(path=d, line=e, basis=f,
                             **({'pointer': p} if p else {}))) for a, b, c, d, e, f, p in sorted(edges)],
                 gaps=sorted(gaps, key=lambda row: (row['path'], row['line'], row['reason'], row['value'])))
    if inventories is not None:
        graph['configured_evidence'] = inventories
    graph['summary'] = dict(files=len(paths), node_kinds=dict(sorted(Counter(n['kind'] for n in nodes.values()).items())),
                            edge_kinds=dict(sorted(Counter(e['kind'] for e in graph['edges']).items())),
                            gap_kinds=dict(sorted(Counter(g['reason'] for g in gaps).items())),
                            authored_variant_cells=dict(sorted(Counter(
                                'pending_binding' if isinstance(cell, dict) else 'assertion_bound'
                                for behavior in ledger['behaviors'] for cells in behavior['variant_cells'].values()
                                for cell in cells.values()).items())))
    return graph


def owner_cards(graph):
    files = {n['id']: n for n in graph['nodes'] if n['kind'] == 'file'}
    behaviors = {n['id']: n for n in graph['nodes'] if n['kind'] == 'behavior'}
    cards = {}
    for node in files.values():
        if node.get('owner'):
            card = cards.setdefault(node['owner'], dict(files=[], characters=0, lines=0, lexical_tokens=0,
                                                       dependencies=set(), contracts=set(), consumers=set(), tests=set(),
                                                       behaviors=set(), authored_tests=set()))
            card['files'].append(node['path'])
            for key in ('characters', 'lines', 'lexical_tokens'):
                card[key] += node[key]
    for edge in graph['edges']:
        if edge['kind'] == 'requires_owner' and edge['source'] in behaviors:
            owner = edge['target'].removeprefix('owner:')
            cards[owner]['behaviors'].add(edge['source'].removeprefix('behavior:'))
            for checks in behaviors[edge['source']].get('checks', {}).values():
                for check in checks:
                    cards[owner]['authored_tests'].update(check.get('files', []))
            continue
        if edge['kind'] != 'references' or edge['source'] not in files or edge['target'] not in files:
            continue
        source, target = files[edge['source']], files[edge['target']]
        left, right = source.get('owner'), target.get('owner')
        if left != right:
            if left:
                cards[left]['dependencies'].add(target['path'])
                if target.get('production'):
                    cards[left]['contracts'].add(target['path'])
            if right:
                cards[right]['consumers'].add(source['path'])
                if source['path'].startswith('test/') or '.test.' in source['path']:
                    cards[right]['tests'].add(source['path'])
    for card in cards.values():
        for key in ('dependencies', 'contracts', 'consumers', 'tests', 'behaviors', 'authored_tests'):
            card[key] = sorted(card[key])
        card['requirements'] = sorted((behaviors['behavior:' + identity]
                                       for identity in card['behaviors']), key=lambda row: row['id'])
        card['direct_packet_characters'] = card['characters'] + sum(files['file:' + p]['characters'] for p in card['contracts'])
        card['test_relation'] = 'literal references only; not measured coverage'
    return dict(sorted(cards.items()))


def task_slice(graph, owner, depth=1, max_files=100):
    cards = owner_cards(graph)
    if owner not in cards or depth < 0 or max_files < 1:
        raise ValueError('unknown owner or invalid slice bounds')
    adjacency = defaultdict(set)
    for edge in graph['edges']:
        if edge['kind'] == 'references':
            adjacency[edge['source']].add(edge['target'])
            adjacency[edge['target']].add(edge['source'])
    seeds = {'file:' + path for path in cards[owner]['files']}
    selected = set(seeds)
    frontier = set(selected)
    for _ in range(depth):
        frontier = set().union(*(adjacency[item] for item in frontier)) - selected
        selected.update(frontier)
    ordered = sorted(seeds) + sorted(selected - seeds)
    requirements = sorted((node for node in graph['nodes'] if node['kind'] == 'behavior' and
                           node['id'].removeprefix('behavior:') in cards[owner]['behaviors']),
                          key=lambda node: node['id'])
    acceptance_files = sorted({path for requirement in requirements
                               for checks in requirement.get('checks', {}).values()
                               for check in checks for path in check.get('files', [])})
    return dict(owner=owner, depth=depth, total_files=len(selected), complete=len(selected) <= max_files,
                files=[item[5:] for item in ordered[:max_files]],
                omitted=[item[5:] for item in ordered[max_files:]],
                requirements=requirements, acceptance_files=acceptance_files,
                scope='bounded undirected literal-reference neighborhood plus complete authored behavior requirements; dynamic paths remain unresolved')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=budget.ROOT)
    parser.add_argument('--output', type=Path, help='write generated JSON outside the source tree')
    parser.add_argument('--check', type=Path, help='refuse a stale saved full graph')
    parser.add_argument('--configured-evidence', type=Path, help='optional outside-tree exact-source inventory manifest')
    parser.add_argument('--summary', action='store_true')
    parser.add_argument('--owners', action='store_true')
    parser.add_argument('--owner')
    parser.add_argument('--depth', type=int, default=1)
    parser.add_argument('--max-files', type=int, default=100)
    args = parser.parse_args()
    try:
        graph = extract(args.root, configured=args.configured_evidence)
        if args.check and json.loads(args.check.read_text()) != graph:
            raise ValueError('stale graph: regenerate from the exact candidate and review changed impact')
        result = (task_slice(graph, args.owner, args.depth, args.max_files) if args.owner else
                  owner_cards(graph) if args.owners else graph['summary'] if args.summary else graph)
        encoded = json.dumps(result, indent=2, sort_keys=True) + '\n'
        if args.output:
            if args.output.resolve().is_relative_to(args.root.resolve()):
                raise ValueError('generated output must remain outside the repository')
            args.output.write_text(encoded)
        else:
            print(encoded, end='')
    except (ValueError, OSError) as error:
        parser.exit(1, f'Repository intelligence: {error}\n')


if __name__ == '__main__':
    main()
