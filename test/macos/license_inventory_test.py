#!/usr/bin/env python3
import importlib.util
import json
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("inventory", ROOT / "script/macos-license-inventory.py")
tool = importlib.util.module_from_spec(spec); spec.loader.exec_module(tool)

with tempfile.TemporaryDirectory(prefix="private host path ") as temporary:
    temporary = Path(temporary)
    root = temporary / "opt/homebrew/Cellar/example/1.2.3"
    lib = root / "lib/libexample.dylib"; lib.parent.mkdir(parents=True); lib.write_bytes(b"macho-fixture")
    brew = root / ".brew"; brew.mkdir()
    recipe = brew / "example.rb"
    recipe.write_text('url "https://example.invalid/source.tar.gz"\nsha256 "' + "a" * 64 + '"\ndepends_on "zlib"\n')
    # Keg-root records may contain private build paths; report summaries must not copy them.
    (root / "INSTALL_RECEIPT.json").write_text(json.dumps({"version": "1.2.3", "built_as_bottle": False,
        "private_path": str(temporary / "secret") }))
    (root / "sbom.spdx.json").write_text(json.dumps({"SPDXID": "SPDXRef-DOCUMENT",
        "creationInfo": {"comment": str(temporary / "host-only")}}))
    # Old incorrect .brew locations cannot satisfy provenance.
    (brew / "INSTALL_RECEIPT.json").write_text('{"wrong":true}')
    (brew / "sbom.spdx.json").write_text('{"wrong":true}')
    (root / "COPYING.RUNTIME").write_text("runtime notice")
    nested = root / "share/licenses/nested"; nested.mkdir(parents=True)
    (nested / "COPYING.LIB").write_text("library notice")
    (nested / "COPYRIGHT").write_text("copyright notice")
    # Deliberately ignored: source-like path is not recursively globbed for notices.
    ignored = root / "src/third_party"; ignored.mkdir(parents=True)
    (ignored / "LICENSE").write_text("must not scan source tree")
    notices = temporary / "notices"
    result = tool.inventory([str(lib)], str(notices))
    component = result["components"][0]
    assert component["formula"] == "example" and component["version"] == "1.2.3"
    assert component["input"] == "lib/libexample.dylib"
    assert component["formula_recipe"]["path"] == ".brew/example.rb"
    assert component["formula_recipe"]["urls"] == ["https://example.invalid/source.tar.gz"]
    assert component["formula_recipe"]["dependencies"] == ["zlib"]
    assert component["install_receipt"]["path"] == "INSTALL_RECEIPT.json"
    assert component["install_receipt"]["json"]["top_level_keys"] == ["built_as_bottle", "private_path", "version"]
    assert component["sbom"]["path"] == "sbom.spdx.json"
    assert {item["path"] for item in component["notice_files"]} == {
        "COPYING.RUNTIME", "share/licenses/nested/COPYING.LIB", "share/licenses/nested/COPYRIGHT"}
    copied = {item["path"] for item in component["copied_files"]}
    assert ".brew/example.rb" in copied
    assert not {"INSTALL_RECEIPT.json", "sbom.spdx.json"} & copied
    assert {"COPYING.RUNTIME", "share/licenses/nested/COPYING.LIB", "share/licenses/nested/COPYRIGHT"} <= copied
    assert (notices / "example-1.2.3/.brew/example.rb").read_bytes() == recipe.read_bytes()
    assert not component["missing_provenance"]
    assert result["redistribution_status"] == "not-reviewed-not-approved"
    assert "resources" in result["provenance_gaps"][1]
    rendered = json.dumps(result, sort_keys=True)
    assert str(temporary) not in rendered and "host-only" not in rendered
    for exported in notices.rglob('*'):
        if exported.is_file(): assert str(temporary).encode() not in exported.read_bytes()
    opt = temporary / 'opt/homebrew/opt/example'
    opt.parent.mkdir(parents=True)
    opt.symlink_to(root)
    resolved = tool.inventory([str(opt / 'lib/libexample.dylib')])['components'][0]
    assert resolved['input'] == component['input'] and resolved['input_sha256'] == tool.sha256(lib)
    # Symlinked recipe and notice roots must not make the sanitized export read
    # or copy material outside the resolved Cellar keg.
    outside = temporary / "outside"; outside.mkdir()
    (outside / "escaped.rb").write_text('url "https://example.invalid/outside"\n')
    (outside / "LICENSE").write_text("outside notice")
    escaped = temporary / "opt/homebrew/Cellar/escaped/1.0"
    escaped_lib = escaped / "lib/escaped.dylib"; escaped_lib.parent.mkdir(parents=True); escaped_lib.write_bytes(b"escaped")
    (escaped / ".brew").symlink_to(outside, target_is_directory=True)
    (escaped / "share").symlink_to(outside, target_is_directory=True)
    escaped_notices = temporary / "escaped-notices"
    escaped_component = tool.inventory([str(escaped_lib)], str(escaped_notices))["components"][0]
    assert not escaped_component["formula_recipe"]["present"]
    assert "formula recipe" in escaped_component["missing_provenance"]
    assert "license/notice file" in escaped_component["missing_provenance"]
    assert not list(escaped_notices.rglob("*.rb")) and not list(escaped_notices.rglob("LICENSE"))
    try: tool.cellar_identity("relative.dylib")
    except ValueError: pass
    else: raise AssertionError("relative input accepted")
print("macOS license inventory fixture passed")
