#!/usr/bin/env python3
"""Bundle identical non-driver DSP libraries; keep each host's Vulkan driver."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("build", type=Path)
args = parser.parse_args()
library_dir = args.build / "lib"
library_dir.mkdir(exist_ok=True)
manifest = {}
for binary in (args.build / "bin").iterdir():
    if not binary.is_file():
        continue
    manifest[binary.name] = hashlib.sha256(binary.read_bytes()).hexdigest()
    result = subprocess.check_output(["ldd", str(binary)], text=True)
    for line in result.splitlines():
        words = line.split()
        if len(words) < 3 or words[1] != "=>" or not words[2].startswith("/"):
            continue
        name, source = words[0], Path(words[2])
        if name.startswith(("libfftw3", "libarmadillo", "libblas.", "liblapack.", "libarpack.", "libsuperlu.", "libgfortran.", "libquadmath.")):
            shutil.copy2(source, library_dir / name)
            manifest["lib/" + name] = hashlib.sha256(source.read_bytes()).hexdigest()
(args.build / "package-sha256.json").write_text(json.dumps(manifest, indent=2) + "\n")
