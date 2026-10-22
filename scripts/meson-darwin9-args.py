#!/usr/bin/env python3
import ast
import configparser
import json
import subprocess
import sys
from pathlib import Path

cross_path, build_path = map(Path, sys.argv[1:3])
cross = configparser.ConfigParser()
cross.read(cross_path)
options = json.loads((build_path / "meson-info/intro-buildoptions.json").read_text())
current = {option["name"]: option["value"] for option in options}
changed = []
for key in ("c_args", "cpp_args", "objc_args", "c_link_args", "cpp_link_args"):
    value = ast.literal_eval(cross["built-in options"][key])
    if current.get(key) != value:
        changed.append("-D" + key + "=" + repr(value))
if changed:
    subprocess.run(
        [sys.executable, "-m", "mesonbuild.mesonmain", "configure", str(build_path), *changed],
        check=True,
    )
