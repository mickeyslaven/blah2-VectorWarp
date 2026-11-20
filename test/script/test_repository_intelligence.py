"""Behavioral negative controls for deterministic source/owner/impact routing."""
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'script'))
SPEC = importlib.util.spec_from_file_location('repository_intelligence', ROOT / 'script/repository_intelligence.py')
RIG = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RIG)


class RepositoryFixture(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.document = {'snapshot': {'exclusions': {}}, 'context_snapshot': {'policy': {
            'owners': [{'owner': 'native', 'patterns': ['src/*']},
                       {'owner': 'api', 'patterns': ['api/*']},
                       {'owner': 'build', 'patterns': ['CMakeLists.txt', 'cmake/*']}]}}}
        self.ledger = {'schema_version': 2, 'behaviors': []}
        self.put('src/a.h', '#pragma once\n')
        self.put('src/main.cpp', '#include "a.h"\n#include <external.h>\n')
        self.put('api/session.js', 'module.exports = {};\n')
        self.put('api/server.js', "const session = require('./session');\napp.get('/api/map', handler);\n")
        self.put('test/session.test.js', "require('../api/session');\n")
        self.put('CMakeLists.txt', 'add_library(core STATIC\n  src/main.cpp)\nadd_executable(app src/main.cpp)\ntarget_link_libraries(app PRIVATE core Threads::Threads)\nadd_test(NAME smoke COMMAND app)\n')

    def put(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)

    def paths(self):
        return [p.relative_to(self.root).as_posix() for p in self.root.rglob('*') if p.is_file() or p.is_symlink()]

    def graph(self, paths=None):
        return RIG.extract(self.root, self.paths() if paths is None else paths, self.document, self.ledger)

    def behavior_fixture(self):
        anchor = 'assert.strictEqual(session.active, true);'
        self.put('test/session.test.js', "require('../api/session');\n" + anchor + '\n')
        behavior = dict(id='session', owners=['api'], contract='preserve sessions',
            required_inputs={'candidate': 'exact candidate'}, variants=['linux', 'darwin'],
            assertions=[dict(id='session.active', facet='positive', outcome='session is active',
                             file='test/session.test.js', anchor=anchor)],
            checks={'positive': [dict(command=['node', 'test/session.test.js'],
                                      files=['test/session.test.js'], assertions=['session.active'])]},
            variant_cells={variant: {facet: ['session.active'] if facet == 'positive' else
                {'pending': 'Needs a dedicated lifecycle assertion'} for facet in RIG.FACETS}
                for variant in ['linux', 'darwin']})
        self.ledger['behaviors'] = [behavior]
        return behavior


class RepositoryIntelligenceControls(RepositoryFixture):

    def test_order_and_mtime_do_not_affect_graph(self):
        first = self.graph()
        (self.root / 'src/main.cpp').touch()
        self.assertEqual(first, self.graph(list(reversed(self.paths()))))

    def test_digest_and_impact_change_with_source_and_deleted_files(self):
        first = self.graph()
        self.put('api/server.js', "require('./session');\nrequire('../src/a.h');\n")
        second = self.graph()
        self.assertNotEqual(first['source_digest'], second['source_digest'])
        self.assertIn('api/server.js', RIG.task_slice(second, 'native')['files'])
        (self.root / 'api/session.js').unlink()
        third = self.graph()
        self.assertNotEqual(second['source_digest'], third['source_digest'])
        self.assertTrue(any(g['value'] == './session' for g in third['gaps']))

    def test_source_mutation_during_extraction_cannot_seal_a_mixed_snapshot(self):
        original = RIG.cmake_commands
        def concurrent_writer(text):
            yield from original(text)
            self.put('src/a.h', '// concurrent edit\n')
        with patch.object(RIG, 'cmake_commands', concurrent_writer):
            with self.assertRaisesRegex(ValueError, 'source changed during extraction'):
                self.graph()

    def test_cmake_multiline_aliases_and_links_are_candidates(self):
        self.put('cmake/other.cmake', 'if(DISABLED)\nadd_library(alias ALIAS core)\nendif()\ntarget_sources(${dynamic} PRIVATE missing.cpp)\n')
        graph = self.graph()
        edges = graph['edges']
        self.assertTrue(any(e['source'] == 'cmake_target_candidate:core' and e['target'] == 'file:src/main.cpp' for e in edges))
        self.assertTrue(any(e['source'] == 'cmake_link:core' and e['target'] == 'cmake_target_candidate:core' for e in edges))
        self.assertTrue(any(g['reason'] == 'computed_cmake_target' for g in graph['gaps']))
        self.assertFalse(any('active' in e['kind'] or 'covers' in e['kind'] for e in edges))
        self.assertEqual(list(RIG.cmake_commands('# add_library(fake x)\nadd_library(real "a(b).cpp")'))[0][1], ['real', 'a(b).cpp'])

    def test_references_and_routes_have_real_line_provenance(self):
        graph = self.graph()
        found = [e for e in graph['edges'] if e['target'] == 'route:GET /api/map']
        self.assertEqual(found[0]['evidence'], {'path': 'api/server.js', 'line': 2, 'basis': 'candidate'})
        for edge in graph['edges']:
            if edge['evidence']['basis'] not in {'policy', 'authored'}:
                source = self.root / edge['evidence']['path']
                self.assertLessEqual(edge['evidence']['line'], len(source.read_text().splitlines()))

    def test_literal_route_loops_are_inventory_edges_and_other_variables_are_gaps(self):
        self.put('api/server.js', """for (const [route, kind] of [
  ['/api/adsb', 'aircraft'], ['/api/adsb/delay-doppler', '/not-a-route']]) {
  app.get(route, handler);
}
for (const route of ['/api/fused', '/api/fused/data'])
  router.post(route, handler);
app.get(computedRoute, handler);
app.get('/api/' + suffix, handler);
app.get(`/api/${kind}`, handler);
for (const route of ['/api/not-exact']) app.get(route + suffix, handler);
for (const route of ['/api/commented', /* don't erase registrations */ dynamicRoute]) {
  app.get(route, handler);
}
for (const [route = '/fallback', metadata] of [[, '/metadata']]) {
  app.get(route, handler);
}
for (const [route, kind] of [['/api/trailing', 'metadata'],]) {
  app.get(route, handler);
}
""")
        graph = self.graph()
        routes = {node['method'] + ' ' + node['route'] for node in graph['nodes']
                  if node['kind'] == 'route_candidate'}
        self.assertEqual(routes, {'GET /api/adsb', 'GET /api/adsb/delay-doppler',
                                  'GET /api/commented', 'GET /api/trailing',
                                  'POST /api/fused', 'POST /api/fused/data'})
        self.assertTrue(any(g['reason'] == 'computed_route' and g['value'] == 'computedRoute'
                            for g in graph['gaps']))
        computed = {g['value'] for g in graph['gaps'] if g['reason'] == 'computed_route'}
        self.assertTrue({"'/api/' + suffix", '`/api/${kind}`', 'route + suffix',
                         "/* don't erase registrations */ dynamicRoute", '<elided-route>'} <= computed)
        self.assertFalse(any(route.endswith('/api/') or route.endswith('/api/not-exact') for route in routes))
        self.assertNotIn('GET /metadata', routes)

    def test_computed_imports_and_external_headers_remain_explicit_gaps(self):
        self.put('api/dynamic.js', 'const a = import(dynamic);\n')
        self.put('test/load.py', 'from missing import example\n')
        graph = self.graph()
        reasons = graph['summary']['gap_kinds']
        self.assertIn('computed_module', reasons)
        self.assertIn('unresolved_or_external_python', reasons)
        self.assertTrue(any(g['value'] == 'external.h' for g in graph['gaps']))

    def test_computed_literal_prefixes_never_create_exact_module_edges(self):
        expressions = ["require('./session' + variant)", "require.resolve('./session' + variant)",
                       "import('./session' + variant)", "require('../api/session.js' + variant)",
                       "require(prefix() + '../api/session.js')",
                       "require(/* ) */ prefix() + '../api/session.js')",
                       "require('./session'\n + variant)"]
        for expression in expressions:
            with self.subTest(expression=expression):
                self.put('api/dynamic.js', expression + ';\n')
                graph = self.graph()
                self.assertFalse(any(e['source'] == 'file:api/dynamic.js' and
                                     e['target'] == 'file:api/session.js' for e in graph['edges']))
                self.assertTrue(any(g['path'] == 'api/dynamic.js' and
                                    g['reason'] == 'computed_module' for g in graph['gaps']))

    def test_template_imports_and_unsupported_string_escapes_are_explicit_gaps(self):
        for expression in ('import(`./${module}.js`)', 'import(`../api/session.js`)',
                           r"require('./sess\u0069on')"):
            with self.subTest(expression=expression):
                self.put('api/dynamic.js', expression + ';\n')
                graph = self.graph()
                self.assertTrue(any(g['path'] == 'api/dynamic.js' and
                                    g['reason'] == 'computed_module' for g in graph['gaps']))
                self.assertFalse(any(e['source'] == 'file:api/dynamic.js' and
                                     e['kind'] == 'references' for e in graph['edges']))

    def test_cmake_hashes_in_quoted_or_escaped_tokens_preserve_arguments_and_lines(self):
        self.assertEqual(list(RIG.cmake_commands('add_library(real "a#b.cpp")\n')),
                         [('add_library', ['real', 'a#b.cpp'], 1)])
        source = '# hidden(\nadd_library(real "a#[=[b]=].cpp" c\\#d.cpp) # ignored(\n'
        self.assertEqual(list(RIG.cmake_commands(source)),
                         [('add_library', ['real', 'a#[=[b]=].cpp', 'c#d.cpp'], 2)])
        source = '#[=[\nadd_library(fake x)\n]=]\nadd_library(real "a#b.cpp")\n'
        self.assertEqual(list(RIG.cmake_commands(source)),
                         [('add_library', ['real', 'a#b.cpp'], 4)])

    def test_owner_cards_and_slices_report_debt_without_truncation_claims(self):
        graph = self.graph()
        cards = RIG.owner_cards(graph)
        self.assertEqual(cards['api']['tests'], ['test/session.test.js'])
        packet = RIG.task_slice(graph, 'api', depth=1, max_files=1)
        self.assertFalse(packet['complete'])
        self.assertEqual(len(packet['files']) + len(packet['omitted']), packet['total_files'])
        with self.assertRaises(ValueError):
            RIG.task_slice(graph, 'missing')

    def test_missing_owner_fails_and_symlinks_cannot_escape(self):
        self.put('tools/unowned.py', 'pass\n')
        with self.assertRaisesRegex(ValueError, 'no canonical production owner'):
            self.graph()
        (self.root / 'tools/unowned.py').unlink()
        (self.root / 'src/outside.h').symlink_to(ROOT / 'AGENTS.md')
        with self.assertRaisesRegex(ValueError, 'external symlink'):
            self.graph()

    def test_production_symlinks_cannot_bypass_owner_accounting(self):
        (self.root / 'api/alias.js').symlink_to('session.js')
        with self.assertRaisesRegex(ValueError, 'production symlink'):
            self.graph()
        (self.root / 'api/alias.js').unlink()
        (self.root / 'src/alias').symlink_to('../api', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'production symlink'):
            RIG.source_target(self.root, 'src/alias/session.js')

    def test_git_discovery_refuses_external_symlink_ancestors_before_reading_them(self):
        self.put(RIG.budget.BASELINE, json.dumps(self.document))
        self.put(RIG.LEDGER, json.dumps(self.ledger))
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        subprocess.run(['git', '-C', str(self.root), 'add', '.'], check=True)
        self.put('.gitignore', 'src\n')
        read_bytes = Path.read_bytes
        def checked_read(path):
            self.assertTrue(path.resolve().is_relative_to(self.root), 'read escaped source root')
            return read_bytes(path)
        with tempfile.TemporaryDirectory() as external:
            target = Path(external) / 'src'
            (self.root / 'src').rename(target)
            (self.root / 'src').symlink_to(target, target_is_directory=True)
            with patch.object(Path, 'read_bytes', checked_read):
                with self.assertRaisesRegex(ValueError, 'external symlink'):
                    RIG.extract(self.root)

    def test_behavior_bindings_require_files_and_never_invent_passes(self):
        authored = self.behavior_fixture()
        graph = self.graph()
        behavior = next(n for n in graph['nodes'] if n['kind'] == 'behavior')
        self.assertEqual(behavior['execution'], 'pending')
        self.assertEqual(graph['summary']['gap_kinds']['missing_behavior_facet'], 3)
        card = RIG.owner_cards(graph)['api']
        self.assertEqual(card['behaviors'], ['session'])
        self.assertEqual(card['authored_tests'], ['test/session.test.js'])
        self.assertEqual(card['requirements'][0]['contract'], 'preserve sessions')
        self.assertEqual(card['requirements'][0]['required_inputs'], {'candidate': 'exact candidate'})
        self.assertIn('checks', card['requirements'][0])
        packet = RIG.task_slice(graph, 'api', max_files=1)
        self.assertEqual([item['id'] for item in packet['requirements']], ['behavior:session'])
        self.assertEqual(packet['acceptance_files'], ['test/session.test.js'])
        self.assertFalse(packet['complete'])
        for payload in (card['requirements'][0], packet['requirements'][0]):
            self.assertEqual(payload, behavior, 'bounded packets retain the entire behavior payload')
            for field in ('assertions', 'variants', 'variant_cells', 'checks'):
                self.assertEqual(payload[field], authored[field])
        assertion = next(n for n in graph['nodes'] if n['kind'] == 'behavior_assertion')
        self.assertEqual(assertion['execution'], 'pending')
        self.assertEqual(graph['summary']['gap_kinds']['unbound_behavior_variant_cell'], 6)
        self.assertEqual(graph['summary']['authored_variant_cells'],
                         {'assertion_bound': 2, 'pending_binding': 6})
        anchor = next(e for e in graph['edges'] if e['kind'] == 'authored_assertion_anchor')
        self.assertEqual(anchor['evidence'], {'path': 'test/session.test.js', 'line': 2, 'basis': 'authored'})
        self.ledger['behaviors'][0]['checks']['positive'][0]['files'] = ['test/missing.js']
        with self.assertRaisesRegex(ValueError, 'missing test'):
            self.graph()

    def test_assertion_schema_rejects_missing_duplicate_or_dangling_bindings(self):
        # Each mutation removes a distinct piece of acceptance evidence.
        mutations = [
            (('assertions',), [], 'missing assertions'),
            (('assertions', 0, 'id'), '', 'assertion identity'),
            (('assertions', 0, 'anchor'), 'missing assertion', 'assertion anchor'),
            (('assertions', 0, 'file'), 'test/missing.js', 'assertion source'),
            (('assertions', 0, 'facet'), 'unsupported', 'assertion facet'),
            (('checks', 'positive', 0, 'assertions'), [], 'assertion references'),
            (('checks', 'positive', 0, 'assertions'), ['unknown'], 'assertion reference'),
            (('checks', 'positive', 0, 'assertions'), ['session.active'] * 2, 'assertion references'),
            (('checks', 'positive', 0, 'files'), ['test/other.test.js'], 'omits assertion source'),
            (('checks', 'positive', 0, 'command'), ['node', 4], 'argv list'),
            (('checks',), {}, 'no command binding'),
            (('variants',), ['linux', 'linux'], 'duplicate required variants'),
            (('variants',), ['*'], 'explicit literals'),
            (('variants',), ['default'], 'explicit literals'),
            (('variant_cells', 'linux', 'positive'), [], 'assertion references'),
            (('variant_cells', 'linux', 'positive'), ['unknown'], 'assertion reference'),
            (('variant_cells', 'linux', 'negative'), ['session.active'], 'wrong-facet'),
            (('variant_cells', 'linux', 'negative'), {'pending': ''}, 'pending cell'),
            (('variant_cells', 'linux', 'positive'), {'alias': 'darwin'}, 'pending cell'),
            (('variant_cells', 'linux', 'positive'), {'execution': 'passed'}, 'pending cell')]
        self.put('test/other.test.js', '// another file is not the assertion source\n')
        for path, replacement, reason in mutations:
            with self.subTest(path=path, replacement=replacement):
                target = self.behavior_fixture()
                for key in path[:-1]:
                    target = target[key]
                target[path[-1]] = replacement
                with self.assertRaisesRegex(ValueError, reason):
                    self.graph()

    def test_assertion_identity_and_required_matrix_cells_cannot_disappear(self):
        for mutation, reason in [
            (lambda b: b['assertions'][0].pop('id'), 'assertion fields'),
            (lambda b: b['assertions'].append(dict(b['assertions'][0])), 'duplicate assertion identity'),
            (lambda b: b['checks']['positive'][0].pop('assertions'), 'command must name assertions'),
            (lambda b: b['checks']['positive'].append({'command': ['true'], 'files': ['test/session.test.js']}),
             'command must name assertions'),
            (lambda b: b.pop('variants'), 'required variants'),
            (lambda b: b['variant_cells'].pop('darwin'), 'required variant mappings'),
            (lambda b: b['variant_cells'].update({'*': b['variant_cells']['linux']}), 'required variant mappings'),
            (lambda b: b['variant_cells']['linux'].pop('negative'), 'facet cells'),
            (lambda b: b['variant_cells']['linux'].update({'default': []}), 'facet cells'),
            (lambda b: b['checks'].update({'negative': b['checks']['positive']}), 'wrong-facet'),
            (lambda b: b['assertions'][0].update(execution='passed'), 'assertion fields')]:
            with self.subTest(reason=reason):
                mutation(self.behavior_fixture())
                with self.assertRaisesRegex(ValueError, reason):
                    self.graph()
        behavior = self.behavior_fixture()
        other = dict(behavior, id='second')
        self.ledger['behaviors'].append(other)
        with self.assertRaisesRegex(ValueError, 'duplicate assertion identity'):
            self.graph()

    def test_duplicate_anchors_and_json_cells_fail_without_execution(self):
        behavior = self.behavior_fixture()
        self.put('test/session.test.js', (behavior['assertions'][0]['anchor'] + '\n') * 2)
        with self.assertRaisesRegex(ValueError, 'ambiguous assertion anchor'):
            self.graph()
        self.behavior_fixture()
        raw = json.dumps(self.ledger).replace('"linux":', '"linux": {}, "linux":')
        self.put(RIG.LEDGER, raw)
        with self.assertRaisesRegex(ValueError, 'duplicate JSON key linux'):
            RIG.extract(self.root, self.paths(), self.document)
        for version in (1, True, 2.0, None):
            self.ledger['schema_version'] = version
            with self.assertRaisesRegex(ValueError, 'unsupported schema'):
                self.graph()

    def test_npm_and_settings_edges_are_literal_not_coverage(self):
        self.put('api/config-rules.js', """const rules = {
  'process.enabled': setting({}),
  'capture.replay.sample_rate_hz': {type: 'number'}
};
""")
        self.put('test/config.test.js', "check('process.enabled');\n")
        self.put('api/package.json', json.dumps({'scripts': {'test': 'node ../test/config.test.js'}}))
        graph = self.graph()
        self.assertTrue(any(e['target'] == 'setting:process.enabled' and e['source'] == 'file:test/config.test.js' for e in graph['edges']))
        self.assertTrue(any(e['target'] == 'setting:capture.replay.sample_rate_hz' and
                            e['source'] == 'file:api/config-rules.js' for e in graph['edges']))
        self.assertTrue(any(e['kind'] == 'runs_candidate' and e['target'] == 'file:test/config.test.js' for e in graph['edges']))

    def test_cli_freshness_and_generated_output_boundary(self):
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        self.put(RIG.budget.BASELINE, json.dumps(self.document))
        self.put(RIG.LEDGER, json.dumps(self.ledger))
        with tempfile.TemporaryDirectory() as packet:
            output = Path(packet) / 'graph.json'
            command = [sys.executable, str(ROOT / 'script/repository_intelligence.py'), '--root', str(self.root)]
            written = subprocess.run(command + ['--output', str(output)], capture_output=True, text=True)
            self.assertEqual(written.returncode, 0, written.stderr)
            checked = subprocess.run(command + ['--check', str(output), '--summary'], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stderr)
            self.put('src/a.h', '// changed\n')
            stale = subprocess.run(command + ['--check', str(output)], capture_output=True, text=True)
            self.assertNotEqual(stale.returncode, 0)
            self.assertIn('stale graph', stale.stderr)
            inside = subprocess.run(command + ['--output', str(self.root / 'generated.json')], capture_output=True, text=True)
            self.assertNotEqual(inside.returncode, 0)
            self.assertFalse((self.root / 'generated.json').exists())

    def test_current_repository_owners_and_seed_ledger_are_resolvable(self):
        graph = RIG.extract(ROOT)
        self.assertGreater(graph['summary']['files'], 0)
        self.assertTrue(all(n.get('owner') for n in graph['nodes'] if n.get('production')))
        self.assertTrue(all(n['execution'] == 'pending' for n in graph['nodes'] if n['kind'] == 'behavior'))
        behaviors = [n for n in graph['nodes'] if n['kind'] == 'behavior']
        assertions = [n for n in graph['nodes'] if n['kind'] == 'behavior_assertion']
        self.assertEqual(len(behaviors), 10)
        self.assertEqual(len(assertions), sum(len(b['assertions']) for b in behaviors))
        self.assertTrue(all(n['execution'] == 'pending' for n in assertions))
        active_owners = {n['id'] for n in graph['nodes'] if n['kind'] == 'owner'}
        bound_owners = {e['target'] for e in graph['edges'] if e['kind'] == 'requires_owner'}
        self.assertEqual(active_owners, bound_owners, 'every active production owner needs a behavior contract')
        factory_keys = json.loads(subprocess.check_output(['node', '-e',
            "process.stdout.write(JSON.stringify(Object.keys(require(process.argv[1])([])).sort()))",
            str(ROOT / 'api/config-rules.js')], text=True))
        graph_keys = sorted(n['name'] for n in graph['nodes'] if n['kind'] == 'setting')
        self.assertEqual(graph_keys, factory_keys, 'source inventory must equal the executable settings factory')

    def test_seed_variant_cells_cannot_relabel_unrelated_fixture_outcomes(self):
        behaviors = {row['id']: row for row in json.loads((ROOT / RIG.LEDGER).read_text())['behaviors']}
        # A future binding may replace pending work, but cannot reuse the known
        # fixture whose input/platform omits the variant being asserted.
        unrelated = [
            ('numerical-stage-lifecycle', 'gap', 'cross_adapter', 'reference-parity'),
            ('artifact-release-identity', 'darwin', 'positive', 'package-members'),
            ('artifact-release-identity', 'darwin', 'reset_failure', 'artifact-identity'),
            ('qualified-gpu-backend', 'gpu-auto', 'negative', 'missing-device-refusal')]
        for behavior, variant, facet, assertion in unrelated:
            with self.subTest(behavior=behavior, variant=variant, facet=facet):
                cell = behaviors[behavior]['variant_cells'][variant][facet]
                self.assertNotIn(behavior + '.' + assertion, cell if isinstance(cell, list) else [])

    def test_auto_admission_refusal_requires_its_native_test_runner(self):
        behavior = next(row for row in json.loads((ROOT / RIG.LEDGER).read_text())['behaviors']
                        if row['id'] == 'qualified-gpu-backend')
        assertion = 'qualified-gpu-backend.auto-resident-module-refusal'
        self.assertEqual(behavior['variant_cells']['gpu-auto']['negative'], [assertion])
        commands = [check['command'] for check in behavior['checks']['negative']
                    if assertion in check['assertions']]
        self.assertTrue(commands, 'AUTO refusal needs a command selecting its native assertion')
        for command in commands:
            self.assertEqual(command[0], 'ctest')
            self.assertIn('--no-tests=error', command)
            self.assertIn('-R', command)
            self.assertRegex('multiIlluminatorRadar', command[command.index('-R') + 1])


class ConfiguredInventoryControls(RepositoryFixture):
    """Synthetic outside-tree artifacts exercise ingestion without CMake or test execution."""

    def fixture(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.packet = Path(temporary.name)
        self.put('.github/workflows/test.yml', 'name: Fixture\njobs:\n  check:\n    runs-on: fixture\n')
        self.document['context_snapshot']['policy']['owners'][2]['patterns'].append('.github/*')
        build = str(self.packet / 'build')
        self.artifacts = {
            'reply/codemodel.json': {'kind': 'codemodel', 'version': {'major': 2, 'minor': 0},
                'paths': {'source': str(self.root), 'build': build},
                'configurations': [{'name': 'Debug', 'targets': [
                    {'id': 'app::id', 'name': 'app', 'jsonFile': 'app.json'},
                    {'id': 'core::id', 'name': 'core', 'jsonFile': 'core.json'}]}]},
            'reply/app.json': {'id': 'app::id', 'name': 'app', 'type': 'EXECUTABLE',
                'sources': [{'path': 'src/main.cpp'}], 'artifacts': [{'path': 'bin/app'}],
                'dependencies': [{'id': 'core::id'}]},
            'reply/core.json': {'id': 'core::id', 'name': 'core', 'type': 'STATIC_LIBRARY',
                'sources': [{'path': 'src/a.h'}]},
            'ctest.json': {'kind': 'ctestInfo', 'version': {'major': 1, 'minor': 0},
                'backtraceGraph': {'files': [str(self.root / 'CMakeLists.txt')],
                                   'nodes': [{'file': 0, 'line': 5}]},
                'tests': [{'name': 'smoke', 'command': [build + '/bin/app'], 'backtrace': 0,
                           'config': 'Debug'}]},
            'ci.json': {'schema_version': 1, 'jobs': [
                {'workflow': '.github/workflows/test.yml', 'job': 'check', 'line': 3}]}}
        self.receipt = {'schema_version': 1, 'source_digest': self.graph()['source_digest'],
                        'source_root': str(self.root), 'ci_jobs': 'ci.json',
                        'cmake': {'codemodel': 'reply/codemodel.json', 'ctest': 'ctest.json',
                                  'configuration': 'Debug', 'build_root': build}}
        self.seal()

    def seal(self):
        self.receipt['artifacts'] = {}
        for name, value in self.artifacts.items():
            path = self.packet / name
            path.parent.mkdir(parents=True, exist_ok=True)
            raw = json.dumps(value).encode()
            path.write_bytes(raw)
            self.receipt['artifacts'][name] = hashlib.sha256(raw).hexdigest()
        (self.packet / 'manifest.json').write_text(json.dumps(self.receipt))

    def configured_graph(self):
        return RIG.extract(self.root, self.paths(), self.document, self.ledger,
                           configured=self.packet / 'manifest.json')

    def test_configured_edges_are_pinned_inventory_not_execution_or_linkage(self):
        self.fixture()
        graph = self.configured_graph()
        self.assertEqual(graph, self.configured_graph())
        kinds = graph['summary']['edge_kinds']
        for kind in ('configured_build_input', 'configured_build_dependency',
                     'configured_test_command', 'configured_backtrace', 'reports_ci_job'):
            self.assertIn(kind, kinds)
        self.assertFalse(any('covers' in kind or kind == 'configured_link' for kind in kinds))
        for node in graph['nodes']:
            if node['kind'] in {'ctest_configured', 'ci_job_candidate'}:
                self.assertEqual(node['execution'], 'pending')
        for edge in graph['edges']:
            if edge['evidence']['basis'] in {'configured', 'reported_ci_inventory'}:
                self.assertTrue(Path(edge['evidence']['path']).is_file())
                self.assertTrue(edge['evidence']['pointer'].startswith('/'))
        self.assertEqual(graph['configured_evidence']['artifacts'], self.receipt['artifacts'])

    def test_stale_source_artifact_and_manifest_are_refused(self):
        self.fixture()
        self.put('src/a.h', '// changed source\n')
        with self.assertRaisesRegex(ValueError, 'stale source digest'):
            self.configured_graph()
        self.receipt['source_digest'] = self.graph()['source_digest']
        self.seal()
        (self.packet / 'ci.json').write_text('{}')
        with self.assertRaisesRegex(ValueError, 'stale artifact'):
            self.configured_graph()
        self.seal()
        original = RIG.budget.digest
        def mutate_manifest(value):
            (self.packet / 'manifest.json').write_text('{}')
            return original(value)
        with patch.object(RIG.budget, 'digest', mutate_manifest):
            with self.assertRaisesRegex(ValueError, 'manifest changed during extraction'):
                self.configured_graph()

    def test_unsupported_or_mismatched_inventory_is_refused(self):
        self.fixture()
        mutations = [
            ('source root mismatch', self.receipt, 'source_root', '/wrong'),
            ('unsupported CMake', self.artifacts['reply/codemodel.json'], 'version', {'major': 3}),
            ('CMake source/build root mismatch', self.artifacts['reply/codemodel.json'], 'paths', {}),
            ('configuration missing', self.receipt['cmake'], 'configuration', 'Release'),
            ('target identity mismatch', self.artifacts['reply/app.json'], 'id', 'wrong'),
            ('unsealed artifact', self.artifacts['reply/codemodel.json']['configurations'][0]['targets'][0], 'jsonFile', 'missing.json'),
            ('unsupported CTest', self.artifacts['ctest.json'], 'version', {'major': 2}),
            ('CTest configuration mismatch', self.artifacts['ctest.json']['tests'][0], 'config', 'Release'),
            ('invalid backtrace index', self.artifacts['ctest.json']['tests'][0], 'backtrace', -1),
            ('invalid backtrace line', self.artifacts['ctest.json']['backtraceGraph']['nodes'][0], 'line', 1000),
            ('CI job source line mismatch', self.artifacts['ci.json']['jobs'][0], 'line', 2)]
        for reason, target, key, value in mutations:
            with self.subTest(reason=reason):
                previous = target[key]
                target[key] = value
                self.seal()
                with self.assertRaisesRegex(ValueError, reason):
                    self.configured_graph()
                target[key] = previous

    def test_external_generated_inputs_and_wrapped_test_commands_remain_gaps(self):
        self.fixture()
        self.artifacts['reply/app.json']['sources'] += [
            {'path': 'src/a.h', 'isGenerated': True}, {'path': '/external/not-read.cpp'}]
        self.artifacts['reply/app.json']['dependencies'].append({'id': 'unknown::id'})
        self.artifacts['ctest.json']['tests'][0]['command'].insert(0, '/usr/bin/env')
        self.seal()
        graph = self.configured_graph()
        kinds = graph['summary']['gap_kinds']
        self.assertEqual(kinds['generated_or_external_build_input'], 2)
        self.assertIn('unresolved_configured_dependency', kinds)
        self.assertIn('unresolved_configured_test_command', kinds)
        self.assertNotIn('configured_test_command', graph['summary']['edge_kinds'])

    def test_duplicate_identities_and_unsealed_or_escaping_artifacts_are_refused(self):
        self.fixture()
        for rows in [self.artifacts['ctest.json']['tests'], self.artifacts['ci.json']['jobs'],
                     self.artifacts['reply/codemodel.json']['configurations'][0]['targets']]:
            rows.append(rows[0])
            self.seal()
            with self.assertRaisesRegex(ValueError, 'duplicate/invalid'):
                self.configured_graph()
            rows.pop()
        self.seal()
        self.receipt['artifacts']['../outside.json'] = 'a' * 64
        (self.packet / 'manifest.json').write_text(json.dumps(self.receipt))
        with self.assertRaisesRegex(ValueError, 'unsafe artifact path'):
            self.configured_graph()
        self.seal()
        (self.packet / 'ci.json').unlink()
        (self.packet / 'ci.json').symlink_to(self.packet / 'ctest.json')
        with self.assertRaisesRegex(ValueError, 'artifact symlink or escape'):
            self.configured_graph()

    def test_malformed_and_duplicate_json_are_refused_without_tracebacks(self):
        self.fixture()
        self.artifacts['ctest.json']['tests'] = [None]
        self.seal()
        with self.assertRaisesRegex(ValueError, 'invalid tests inventory'):
            self.configured_graph()
        raw = b'{"schema_version": 1, "schema_version": 1, "jobs": []}'
        (self.packet / 'ci.json').write_bytes(raw)
        self.receipt['artifacts']['ci.json'] = hashlib.sha256(raw).hexdigest()
        self.receipt.pop('cmake')
        (self.packet / 'manifest.json').write_text(json.dumps(self.receipt))
        with self.assertRaisesRegex(ValueError, 'duplicate JSON key'):
            self.configured_graph()

    def test_artifact_mutation_during_ingestion_is_refused(self):
        self.fixture()
        original = RIG.json.loads
        def mutate_artifact(raw, **kwargs):
            value = original(raw, **kwargs)
            if isinstance(value, dict) and 'jobs' in value:
                (self.packet / 'ci.json').write_text('{}')
            return value
        with patch.object(RIG.json, 'loads', mutate_artifact):
            with self.assertRaisesRegex(ValueError, 'artifact changed during extraction'):
                self.configured_graph()

    def test_ci_source_line_in_another_yaml_block_is_refused(self):
        self.fixture()
        self.put('.github/workflows/test.yml', 'unrelated:\n  check:\n    value: fixture\n')
        self.receipt['source_digest'] = self.graph()['source_digest']
        self.artifacts['ci.json']['jobs'][0]['line'] = 2
        self.seal()
        with self.assertRaisesRegex(ValueError, 'outside a literal jobs block'):
            self.configured_graph()

    def test_ci_multiline_scalars_and_duplicate_source_jobs_are_refused(self):
        self.fixture()
        for quote in ('"', "'"):
            with self.subTest(quote=quote):
                self.put('.github/workflows/test.yml', 'name: ' + quote + 'Fixture\njobs:\n  check:\n' +
                         quote + '\non: push\njobs:\n  real:\n    runs-on: fixture\n')
                self.receipt['source_digest'] = self.graph()['source_digest']
                self.seal()
                with self.assertRaisesRegex(ValueError, 'outside a literal jobs block'):
                    self.configured_graph()
        self.put('.github/workflows/test.yml',
                 'name: Fixture\njobs:\n  check:\n    runs-on: one\n  check:\n    runs-on: two\n')
        self.receipt['source_digest'] = self.graph()['source_digest']
        self.seal()
        with self.assertRaisesRegex(ValueError, 'duplicate CI source job'):
            self.configured_graph()
        for quote in ('"', "'"):
            for workflow, line in [
                    ('name: Fixture\njobs:\n  real:\n    runs-on: fixture\n' + quote + 'env' + quote + ':\n  check:\n', 6),
                    ('name: Fixture\njobs:\n  check:\n    runs-on: one\n  ' + quote + 'check' + quote + ':\n    runs-on: two\n', 3)]:
                with self.subTest(quote=quote, line=line):
                    self.put('.github/workflows/test.yml', workflow)
                    self.artifacts['ci.json']['jobs'][0]['line'] = line
                    self.receipt['source_digest'] = self.graph()['source_digest']
                    self.seal()
                    with self.assertRaisesRegex(ValueError, 'unsupported quoted CI mapping key'):
                        self.configured_graph()
        for unsupported in ('  !!str check:', '  check: {runs-on: duplicate}', '  ? check'):
            with self.subTest(unsupported=unsupported):
                self.put('.github/workflows/test.yml', 'name: Fixture\njobs:\n  check:\n    runs-on: one\n' + unsupported + '\n')
                self.artifacts['ci.json']['jobs'][0]['line'] = 3
                self.receipt['source_digest'] = self.graph()['source_digest']
                self.seal()
                with self.assertRaisesRegex(ValueError, 'unsupported CI job declaration'):
                    self.configured_graph()

    def test_ctest_dotdot_through_symlink_never_matches_another_artifact(self):
        self.fixture()
        build = Path(self.receipt['cmake']['build_root'])
        (build / 'bin').mkdir(parents=True)
        other = self.packet / 'other'
        (other / 'sub').mkdir(parents=True)
        (build / 'alias').symlink_to(other / 'sub', target_is_directory=True)
        command = str(build / 'alias/../bin/app')
        self.assertNotEqual(Path(command).resolve(), (build / 'bin/app').resolve())
        self.artifacts['ctest.json']['tests'][0]['command'] = [command]
        self.seal()
        graph = self.configured_graph()
        self.assertNotIn('configured_test_command', graph['summary']['edge_kinds'])
        self.assertIn('unresolved_configured_test_command', graph['summary']['gap_kinds'])

    def test_build_root_resolving_into_source_is_refused(self):
        self.fixture()
        Path(self.receipt['cmake']['build_root']).symlink_to(self.root, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'invalid configuration/build root'):
            self.configured_graph()

    def test_schema_and_version_numbers_require_integers_not_booleans_or_floats(self):
        self.fixture()
        mutations = [(self.receipt, 'schema_version', True), (self.receipt, 'schema_version', 1.0),
                     (self.artifacts['ci.json'], 'schema_version', True),
                     (self.artifacts['ci.json'], 'schema_version', 1.0),
                     (self.artifacts['reply/codemodel.json']['version'], 'major', 2.0),
                     (self.artifacts['reply/codemodel.json']['version'], 'minor', False),
                     (self.artifacts['ctest.json']['version'], 'major', True),
                     (self.artifacts['ctest.json']['version'], 'major', 1.0),
                     (self.artifacts['ctest.json']['version'], 'minor', False)]
        for target, key, value in mutations:
            with self.subTest(key=key, value=value, target=target):
                previous = target[key]
                target[key] = value
                self.seal()
                with self.assertRaisesRegex(ValueError, 'unsupported'):
                    self.configured_graph()
                target[key] = previous

    def test_malformed_target_fields_are_refused(self):
        self.fixture()
        target = self.artifacts['reply/app.json']
        for key, value in [('sources', {}), ('sources', [None]), ('sources', [{'path': None}]),
                           ('sources', [{'path': 'src/a.h', 'isGenerated': 1}]),
                           ('dependencies', {}), ('dependencies', [{'id': None}]),
                           ('artifacts', {}), ('artifacts', [{'path': None}]),
                           ('type', ['EXECUTABLE']), ('type', None), ('type', 'invalid')]:
            with self.subTest(key=key, value=value):
                previous = target[key]
                target[key] = value
                self.seal()
                with self.assertRaisesRegex(ValueError, 'invalid'):
                    self.configured_graph()
                target[key] = previous

    def test_selected_configuration_and_exact_executable_association(self):
        self.fixture()
        model = self.artifacts['reply/codemodel.json']
        model['configurations'].append({'name': 'Release', 'targets': [
            {'id': 'app::id', 'name': 'app', 'jsonFile': 'unsealed-release.json'}]})
        self.artifacts['ctest.json']['tests'][0]['command'] = ['app']
        self.seal()
        graph = self.configured_graph()
        configured = [node for node in graph['nodes'] if node['kind'] == 'cmake_target_configured']
        self.assertEqual(len(configured), 2)
        self.assertTrue(all(node['configuration'] == 'Debug' for node in configured))
        self.assertNotIn('configured_test_command', graph['summary']['edge_kinds'])
        self.assertIn('unresolved_configured_test_command', graph['summary']['gap_kinds'])

    def test_cli_saved_graph_checks_include_selected_artifacts(self):
        self.fixture()
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        self.put(RIG.budget.BASELINE, json.dumps(self.document))
        self.put(RIG.LEDGER, json.dumps(self.ledger))
        self.receipt['source_digest'] = RIG.extract(self.root)['source_digest']
        self.seal()
        command = [sys.executable, str(ROOT / 'script/repository_intelligence.py'), '--root', str(self.root),
                   '--configured-evidence', str(self.packet / 'manifest.json')]
        output = self.packet / 'graph.json'
        written = subprocess.run(command + ['--output', str(output)], capture_output=True, text=True)
        self.assertEqual(written.returncode, 0, written.stderr)
        checked = subprocess.run(command + ['--check', str(output), '--summary'], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.artifacts['ctest.json']['tests'][0]['command'].append('--changed')
        self.seal()
        stale = subprocess.run(command + ['--check', str(output)], capture_output=True, text=True)
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn('stale graph', stale.stderr)
        (self.packet / 'ctest.json').write_text('{}')
        stale = subprocess.run(command + ['--summary'], capture_output=True, text=True)
        self.assertNotEqual(stale.returncode, 0)
        self.assertIn('stale artifact', stale.stderr)
        self.put('.github/workflows/test.yml',
                 'name: "Fixture\njobs:\n  check:\n"\njobs:\n  real:\n    runs-on: fixture\n')
        self.receipt['source_digest'] = RIG.extract(self.root)['source_digest']
        self.seal()
        phantom = subprocess.run(command + ['--summary'], capture_output=True, text=True)
        self.assertNotEqual(phantom.returncode, 0)
        self.assertIn('outside a literal jobs block', phantom.stderr)
        for workflow, line in [
                ('name: Fixture\njobs:\n  real:\n    runs-on: fixture\n"env":\n  check:\n', 6),
                ('name: Fixture\njobs:\n  check:\n    runs-on: one\n  "check":\n    runs-on: two\n', 3)]:
            self.put('.github/workflows/test.yml', workflow)
            self.artifacts['ci.json']['jobs'][0]['line'] = line
            self.receipt['source_digest'] = RIG.extract(self.root)['source_digest']
            self.seal()
            unsupported = subprocess.run(command + ['--summary'], capture_output=True, text=True)
            self.assertNotEqual(unsupported.returncode, 0)
            self.assertIn('unsupported quoted CI mapping key', unsupported.stderr)

    def test_ci_only_and_cmake_only_are_optional_and_empty_receipts_fail(self):
        self.fixture()
        saved = self.receipt.pop('cmake')
        artifacts = self.artifacts
        self.artifacts = {'ci.json': artifacts['ci.json']}
        self.seal()
        self.assertIn('ci_job_candidate', self.configured_graph()['summary']['node_kinds'])
        self.receipt.pop('ci_jobs')
        self.receipt['cmake'] = saved
        self.artifacts = {name: value for name, value in artifacts.items() if name != 'ci.json'}
        self.seal()
        self.assertNotIn('ci_job_candidate', self.configured_graph()['summary']['node_kinds'])
        self.receipt.pop('cmake')
        self.seal()
        with self.assertRaisesRegex(ValueError, 'no inventories selected'):
            self.configured_graph()


if __name__ == '__main__':
    unittest.main()
