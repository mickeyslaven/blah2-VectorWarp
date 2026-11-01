#!/usr/bin/env python3
import gzip, hashlib, importlib.util, io, json, sys, tarfile, tempfile, textwrap, unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("inputs", ROOT / "script/prepare-macos-release-inputs.py")
inputs = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(inputs)
EXPORT_SPEC = importlib.util.spec_from_file_location("exporter", ROOT / "script/export-macos-source-closure.py")
exporter = importlib.util.module_from_spec(EXPORT_SPEC); EXPORT_SPEC.loader.exec_module(exporter)

class SourceClosureTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.root = Path(self.tmp.name)
        self.source = "a" * 40; self.run = "1234"; self.arch = "arm64"
        self.runtime = self.root / "standalone.json"
        self.runtime.write_text(json.dumps({"schema":1,"source_id":self.source,"arch":self.arch})+"\n")
        recipe = hashlib.sha256(b"recipe").hexdigest(); source = hashlib.sha256(b"payload").hexdigest()
        input_hash = "4" * 64
        self.inventory = self.root / "inventory.json"
        self.inventory.write_text(json.dumps({"schema":1,"source_id":self.source,"arch":self.arch,
            "components":[{"formula":"demo","version":"1","input":"lib/demo",
                "input_sha256":input_hash,"formula_recipe":{"sha256":recipe,"source_sha256":[source]},
                "notice_files":[],"copied_files":[]}]}))
        self.formulas = {"demo|1|lib/demo":{"recipe_sha256":recipe,"source_sha256":[source],
            "notices":[],"input_sha256":input_hash}}

    def tearDown(self): self.tmp.cleanup()

    def test_workflow_uses_official_core_metadata_for_local_patches(self):
        workflow = (ROOT / ".github/workflows/macos-standalone.yml").read_text()
        self.assertIn("https://formulae.brew.sh/api/formula/", workflow)
        self.assertIn("without_bottle_block", workflow)
        self.assertIn("cannot bind frozen Homebrew formula revision", workflow)
        self.assertIn("ambiguous frozen Homebrew patch history", workflow)
        self.assertIn('"local_patch_revisions"', workflow)
        self.assertIn('local_patch_checksums": checksums', workflow)
        self.assertIn('identifier = "vectorwarp/release-build/vectorwarp-uhd"', workflow)
        exporter_source = (ROOT / "script/export-homebrew-formula-inputs.rb").read_text()
        self.assertIn('if has_local_patch &&', exporter_source)
        self.assertIn('Homebrew API local-patch identity differs', exporter_source)

    def workflow_patch_binder(self):
        workflow = (ROOT / ".github/workflows/macos-standalone.yml").read_text()
        block = workflow.split(
            'python3 - "$raw_specs" "$specs" "$(brew --cellar)" "$api_metadata" <<\'PY\'\n',
            1,
        )[1].split("\n          PY", 1)[0]
        source = textwrap.dedent(block)
        prefix = source.split("values = {}", 1)[0]
        namespace = {}
        with mock.patch.object(sys, "argv", ["binder", "/tmp/a", "/tmp/b", "/tmp/c", "/tmp/d"]):
            exec(prefix, namespace)
        return namespace

    def test_local_patch_history_rejects_ambiguous_patch_bytes(self):
        namespace = self.workflow_patch_binder()
        installed = 'class Demo < Formula\n  url "https://example.invalid/demo.tar.gz"\n  sha256 "' + "a" * 64 + '"\n  patch do\n    file "Patches/demo/fix.patch"\n  end\nend\n'
        formula_path = "Formula/d/demo.rb"
        revisions = ("1" * 40, "2" * 40)
        def fetch(url):
            if "formulae.brew.sh" in url:
                return json.dumps({"ruby_source_path": formula_path, "tap_git_head": revisions[0]}).encode()
            if "api.github.com" in url:
                return json.dumps([{"sha": revisions[1]}]).encode()
            if url.endswith(formula_path):
                return installed.encode()
            if revisions[0] in url:
                return b"original patch"
            if revisions[1] in url:
                return b"different patch"
            raise AssertionError(url)
        with tempfile.TemporaryDirectory() as temporary:
            recipe = Path(temporary) / "demo.rb"; recipe.write_text(installed)
            namespace["fetch"] = fetch
            with self.assertRaisesRegex(SystemExit, "ambiguous frozen Homebrew patch history"):
                namespace["bind_local_patches"]("demo", "1.0", recipe, {})

    def test_local_patch_history_accepts_only_one_patch_identity(self):
        namespace = self.workflow_patch_binder()
        installed = 'class Demo < Formula\n  url "https://example.invalid/demo.tar.gz"\n  sha256 "' + "a" * 64 + '"\n  patch do\n    file "Patches/demo/fix.patch"\n  end\nend\n'
        formula_path = "Formula/d/demo.rb"
        revisions = ("1" * 40, "2" * 40)
        def fetch(url):
            if "formulae.brew.sh" in url:
                return json.dumps({"ruby_source_path": formula_path, "tap_git_head": revisions[0]}).encode()
            if "api.github.com" in url:
                return json.dumps([{"sha": revisions[1]}]).encode()
            if url.endswith(formula_path):
                return installed.encode()
            if "Patches/demo/fix.patch" in url:
                return b"same reviewed patch"
            raise AssertionError(url)
        with tempfile.TemporaryDirectory() as temporary:
            recipe = Path(temporary) / "demo.rb"; recipe.write_text(installed)
            namespace["fetch"] = fetch
            result = namespace["bind_local_patches"]("demo", "1.0", recipe, {})
        self.assertEqual(result["local_patch_revisions"], list(revisions))
        self.assertEqual(
            result["local_patch_checksums"]["Patches/demo/fix.patch"],
            hashlib.sha256(b"same reviewed patch").hexdigest(),
        )

    def test_closure_does_not_bypass_source_claim_transition(self):
        candidate = {"source_id": self.source, "arch": self.arch,
            "runtime_manifest_sha256": inputs.sha256(self.runtime), "components": [],
            "gaps": [inputs.NODE_GAP]}
        baseline = {"source_id": inputs.BASELINE_SOURCE_ID, "arch": self.arch, "components": []}
        candidate_path = self.root / "candidate.json"; candidate_path.write_text(json.dumps(candidate))
        baseline_path = self.root / "baseline.json"; baseline_path.write_text(json.dumps(baseline))
        with mock.patch.object(inputs, "verify_source_claim_transition",
                side_effect=ValueError("transition rejected")) as transition:
            with self.assertRaisesRegex(ValueError, "transition rejected"):
                inputs.verify_inventory(
                    candidate_path, baseline_path, self.root, self.source, self.arch,
                    closure={"schema": 1},
                )
        transition.assert_called_once()

    def closure(self, *, arch=None, omit=False, tamper=False):
        path = self.root / ("closure-" + str(len(list(self.root.glob('closure-*')))) + ".tar.gz")
        payload = b"payload"; recipe = b"recipe"
        record={"sha256":hashlib.sha256(payload).hexdigest(),"size":len(payload)}
        recipe_record={"sha256":hashlib.sha256(recipe).hexdigest(),"size":len(recipe)}
        formula_inputs={"schema":2,"source_id":self.source,"run_id":self.run,"arch":arch or self.arch,
            "brew_version":"test","formulas":[{"id":"demo|1","formula":"demo","version":"1","usage":["runtime"],
                "recipe":{"path":"recipes/demo/1/formula.rb",**recipe_record},"dependencies":[],"inputs":[{
                    "id":"primary","kind":"archive","urls":["https://example.invalid/source"],"checksum":None,"revision":"a"*40,
                    "payload":{"path":"source-cache/demo/1/primary/source",**record}}]}]}
        formula_bytes=(json.dumps(formula_inputs,sort_keys=True)+"\n").encode()
        manifest={"schema":1,"source_id":self.source,"run_id":self.run,"arch":arch or self.arch,
            "runtime_manifest_sha256":inputs.sha256(self.runtime),
            "runtime_formulas":{"demo|1|lib/demo":{"recipe_sha256":recipe_record["sha256"],"notices":[],"input_sha256":"4"*64}},
            "formula_inputs":formula_inputs,
            "gaps_resolved":["node-pseudo-input","boost-source-license","sqlite-public-domain"],
            "trees":{"source-cache":{} if omit else {"demo/1/primary/source":record},"recipes":{"demo/1/formula.rb":recipe_record},"notices":{}},
            "files":{"formula-inputs.json":{"sha256":hashlib.sha256(formula_bytes).hexdigest(),"size":len(formula_bytes)}}}
        value=(json.dumps(manifest,sort_keys=True)+"\n").encode()
        with path.open("xb") as raw,gzip.GzipFile(fileobj=raw,mode="wb",filename="",mtime=0) as gz:
            with tarfile.open(fileobj=gz,mode="w|") as archive:
                info=tarfile.TarInfo("VectorWarp-source-closure/MANIFEST.json");info.size=len(value);archive.addfile(info,io.BytesIO(value))
                info=tarfile.TarInfo("VectorWarp-source-closure/formula-inputs.json");info.size=len(formula_bytes);archive.addfile(info,io.BytesIO(formula_bytes))
                if not omit:
                    body=b"changed" if tamper else payload;info=tarfile.TarInfo("VectorWarp-source-closure/source-cache/demo/1/primary/source");info.size=len(body);archive.addfile(info,io.BytesIO(body))
                info=tarfile.TarInfo("VectorWarp-source-closure/recipes/demo/1/formula.rb");info.size=len(recipe);archive.addfile(info,io.BytesIO(recipe))
        return path

    def test_accepts_exact_candidate_closure(self):
        result=inputs.verify_source_closure(self.closure(),self.inventory,self.runtime,self.source,self.arch,self.run)
        self.assertEqual(result["arch"],self.arch)

    def test_rejects_arch_swap(self):
        with self.assertRaisesRegex(ValueError,"bind the candidate"):
            inputs.verify_source_closure(self.closure(arch="x86_64"),self.inventory,self.runtime,self.source,self.arch,self.run)

    def test_rejects_missing_or_tampered_member(self):
        with self.assertRaisesRegex(ValueError,"payload member differs"):
            inputs.verify_source_closure(self.closure(omit=True),self.inventory,self.runtime,self.source,self.arch,self.run)
        with self.assertRaisesRegex(ValueError,"hash differs"):
            inputs.verify_source_closure(self.closure(tamper=True),self.inventory,self.runtime,self.source,self.arch,self.run)

class SourceClosureExporterTest(unittest.TestCase):
    def test_workflow_uses_evaluated_frozen_recipes(self):
        workflow=(ROOT/".github/workflows/macos-standalone.yml").read_text()
        self.assertIn("export-homebrew-formula-inputs.rb",workflow)
        self.assertIn('source = cellar / formula / version / ".brew"',workflow)
        self.assertNotIn("brew trust vectorwarp/release-build",workflow)
        self.assertNotIn('brew fetch "${fetch_flags[@]}"',workflow)

    def test_exports_exact_payloads_and_rejects_omission(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); inputs=root/"inputs"; cache=inputs/"source-cache"; recipes=inputs/"recipes"; notices=root/"notices"
            for path in (cache,recipes,notices): path.mkdir(parents=True)
            source_id="a"*40; formula_records=[]; demo_recipe=None; demo_source=None
            formulas=[("demo","1",["runtime"]),
                *((name,"1",["compile-header"]) for name in ("asio","cpp-httplib","eigen","rapidjson","vulkan-headers"))]
            for formula,version,usage in formulas:
                recipe=b"recipe-"+formula.encode(); recipe_path=recipes/formula/version/"formula.rb"
                recipe_path.parent.mkdir(parents=True); recipe_path.write_bytes(recipe)
                source_path=cache/formula/version/"primary"/"source.tar.gz"; source_path.parent.mkdir(parents=True)
                if formula == "demo":
                    with source_path.open("wb") as raw,gzip.GzipFile(fileobj=raw,mode="wb",filename="",mtime=0) as gz:
                        with tarfile.open(fileobj=gz,mode="w|") as archive:
                            value=b"Boost Software License - Version 1.0"; info=tarfile.TarInfo("boost/LICENSE_1_0.txt");info.size=len(value);archive.addfile(info,io.BytesIO(value))
                    demo_recipe=recipe; demo_source=source_path
                else: source_path.write_bytes(b"source-"+formula.encode())
                formula_records.append({"id":f"{formula}|{version}","formula":formula,"version":version,"usage":usage,
                    "recipe":{"path":f"recipes/{formula}/{version}/formula.rb","sha256":hashlib.sha256(recipe).hexdigest(),"size":len(recipe)},
                    "dependencies":[],"inputs":[{"id":"primary","kind":"archive","urls":["https://example.invalid/source"],
                        "checksum":{"algorithm":"sha256","value":hashlib.sha256(source_path.read_bytes()).hexdigest()},"revision":None,
                        "payload":{"path":f"source-cache/{formula}/{version}/primary/source.tar.gz","sha256":hashlib.sha256(source_path.read_bytes()).hexdigest(),"size":source_path.stat().st_size}}]})
            formula_inputs=root/"formula-inputs.json"
            formula_inputs.write_text(json.dumps({"schema":2,"arch":"arm64","source_id":source_id,"run_id":"7","brew_version":"test","formulas":formula_records}))
            recipe=demo_recipe; source=demo_source.read_bytes()
            (notices/"LICENSE").write_text("notice")
            inventory=root/"inventory.json"; runtime=root/"standalone.json"
            inventory.write_text(json.dumps({"schema":1,"source_id":source_id,"arch":"arm64","components":[{
                "formula":"demo","version":"1","input":"lib/demo","input_sha256":"4"*64,
                "formula_recipe":{"sha256":hashlib.sha256(recipe).hexdigest(),"source_sha256":[hashlib.sha256(source).hexdigest()]},
                "notice_files":[],"copied_files":[]}],"gaps":[{"index":0,"reason":"dependency lacks formula, version, file, or sha256"},
                {"formula":"boost","reason":"missing license/notice file"},{"formula":"sqlite","reason":"missing license/notice file"}]}))
            runtime.write_text(json.dumps({"schema":1,"source_id":source_id,"arch":"arm64"}))
            fixed=[]
            for name in ("headers","node","vkfft","sqlite"):
                path=root/name;path.write_bytes(name.encode());fixed.append(path)
            exporter.SQLITE_NOTICE_SHA256=hashlib.sha256(fixed[3].read_bytes()).hexdigest()
            output=root/"closure.tar.gz"
            argv=["export", "--inventory",str(inventory),"--runtime-manifest",str(runtime),"--formula-inputs",str(formula_inputs),"--source-cache",str(cache),
                "--recipes",str(recipes),"--notices",str(notices),"--header-versions",str(fixed[0]),"--node-source",str(fixed[1]),
                "--vkfft-source",str(fixed[2]),"--sqlite-notice",str(fixed[3]),"--output",str(output),
                "--arch","arm64","--source-id",source_id,"--run-id","7"]
            old=sys.argv
            try: sys.argv=argv; exporter.main()
            finally: sys.argv=old
            self.assertTrue(output.is_file())
            demo_source.unlink(); output.unlink()
            with self.assertRaisesRegex(ValueError,"payload ownership differs"):
                try: sys.argv=argv; exporter.main()
                finally: sys.argv=old

if __name__ == "__main__": unittest.main()
