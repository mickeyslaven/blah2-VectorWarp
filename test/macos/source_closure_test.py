#!/usr/bin/env python3
import gzip, hashlib, importlib.util, io, json, sys, tarfile, tempfile, unittest
from pathlib import Path

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
        self.assertIn('urllib.parse.quote(formula, safe="")', workflow)
        self.assertIn('if formula == "vectorwarp-uhd":', workflow)

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
