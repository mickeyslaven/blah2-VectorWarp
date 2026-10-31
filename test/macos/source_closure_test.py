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
        notice = "3" * 64; input_hash = "4" * 64
        self.inventory = self.root / "inventory.json"
        self.inventory.write_text(json.dumps({"schema":1,"source_id":self.source,"arch":self.arch,
            "components":[{"formula":"demo","version":"1","input":"lib/demo",
                "input_sha256":input_hash,"formula_recipe":{"sha256":recipe,"source_sha256":[source]},
                "notice_files":[{"path":"LICENSE","sha256":notice}],"copied_files":[]}]}))
        self.formulas = {"demo|1|lib/demo":{"recipe_sha256":recipe,"source_sha256":[source],
            "notices":[["LICENSE",notice]],"input_sha256":input_hash}}

    def tearDown(self): self.tmp.cleanup()

    def closure(self, *, arch=None, omit=False, tamper=False):
        path = self.root / ("closure-" + str(len(list(self.root.glob('closure-*')))) + ".tar.gz")
        payload = b"payload"; recipe = b"recipe"
        record={"sha256":hashlib.sha256(payload).hexdigest(),"size":len(payload)}
        recipe_record={"sha256":hashlib.sha256(recipe).hexdigest(),"size":len(recipe)}
        manifest={"schema":1,"source_id":self.source,"run_id":self.run,"arch":arch or self.arch,
            "runtime_manifest_sha256":inputs.sha256(self.runtime),"formulas":self.formulas,
            "gaps_resolved":["node-pseudo-input","boost-source-license","sqlite-public-domain"],
            "trees":{"source-cache":{} if omit else {"source":record},"recipes":{"demo.rb":recipe_record},"notices":{}},"files":{}}
        value=(json.dumps(manifest,sort_keys=True)+"\n").encode()
        with path.open("xb") as raw,gzip.GzipFile(fileobj=raw,mode="wb",filename="",mtime=0) as gz:
            with tarfile.open(fileobj=gz,mode="w|") as archive:
                info=tarfile.TarInfo("VectorWarp-source-closure/MANIFEST.json");info.size=len(value);archive.addfile(info,io.BytesIO(value))
                if not omit:
                    body=b"changed" if tamper else payload;info=tarfile.TarInfo("VectorWarp-source-closure/source-cache/source");info.size=len(body);archive.addfile(info,io.BytesIO(body))
                info=tarfile.TarInfo("VectorWarp-source-closure/recipes/demo.rb");info.size=len(recipe);archive.addfile(info,io.BytesIO(recipe))
        return path

    def test_accepts_exact_candidate_closure(self):
        result=inputs.verify_source_closure(self.closure(),self.inventory,self.runtime,self.source,self.arch,self.run)
        self.assertEqual(result["arch"],self.arch)

    def test_rejects_arch_swap(self):
        with self.assertRaisesRegex(ValueError,"bind the candidate"):
            inputs.verify_source_closure(self.closure(arch="x86_64"),self.inventory,self.runtime,self.source,self.arch,self.run)

    def test_rejects_missing_or_tampered_member(self):
        with self.assertRaisesRegex(ValueError,"omits"):
            inputs.verify_source_closure(self.closure(omit=True),self.inventory,self.runtime,self.source,self.arch,self.run)
        with self.assertRaisesRegex(ValueError,"hash differs"):
            inputs.verify_source_closure(self.closure(tamper=True),self.inventory,self.runtime,self.source,self.arch,self.run)

class SourceClosureExporterTest(unittest.TestCase):
    def test_exports_exact_payloads_and_rejects_omission(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary); cache=root/"cache"; recipes=root/"recipes"; notices=root/"notices"
            for path in (cache,recipes,notices): path.mkdir()
            source=b"source"; recipe=b"recipe"; (cache/"source.tar.gz").write_bytes(source); (recipes/"demo.rb").write_bytes(recipe)
            boost=cache/"boost-source.tar.gz"
            with boost.open("wb") as raw,gzip.GzipFile(fileobj=raw,mode="wb",filename="",mtime=0) as gz:
                with tarfile.open(fileobj=gz,mode="w|") as archive:
                    value=b"Boost Software License - Version 1.0"; info=tarfile.TarInfo("boost/LICENSE_1_0.txt");info.size=len(value);archive.addfile(info,io.BytesIO(value))
            (notices/"LICENSE").write_text("notice")
            source_id="a"*40; inventory=root/"inventory.json"; runtime=root/"standalone.json"
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
            argv=["export", "--inventory",str(inventory),"--runtime-manifest",str(runtime),"--source-cache",str(cache),
                "--recipes",str(recipes),"--notices",str(notices),"--header-versions",str(fixed[0]),"--node-source",str(fixed[1]),
                "--vkfft-source",str(fixed[2]),"--sqlite-notice",str(fixed[3]),"--output",str(output),
                "--arch","arm64","--source-id",source_id,"--run-id","7"]
            old=sys.argv
            try: sys.argv=argv; exporter.main()
            finally: sys.argv=old
            self.assertTrue(output.is_file())
            (cache/"source.tar.gz").unlink(); output.unlink()
            with self.assertRaisesRegex(ValueError,"omits"):
                try: sys.argv=argv; exporter.main()
                finally: sys.argv=old

if __name__ == "__main__": unittest.main()
