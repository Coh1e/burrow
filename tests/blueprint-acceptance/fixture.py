"""Prepare an independent Burrow environment using the product's public CLI."""

import hashlib
import os
import shutil
from pathlib import Path


def create_catalog(directory):
    directory.mkdir()
    if os.name == "nt":
        suffix = ".cmd"
        scripts = {
            ("builder", "1"): "@echo 1\r\n",
            ("builder", "2"): '@echo off\r\nset "BRW_ACCEPT_BUILDER=2"\r\nmiddle.cmd %*\r\n',
            ("middle", "2"): "@base.cmd %*\r\n",
            ("base", "2"): "@python %*\r\n@exit /b %errorlevel%\r\n",
        }
    else:
        suffix = ""
        scripts = {
            ("builder", "1"): "#!/bin/sh\necho 1\n",
            ("builder", "2"): '#!/bin/sh\nexport BRW_ACCEPT_BUILDER=2\nexec middle "$@"\n',
            ("middle", "2"): '#!/bin/sh\nexec base "$@"\n',
            ("base", "2"): '#!/bin/sh\nexec python "$@"\n',
        }

    dependencies = {("builder", "2"): "middle", ("middle", "2"): "base"}
    rows = ["brw-catalog-v1"]
    for (name, version), script in scripts.items():
        data = script.encode("ascii")
        artifact = directory / f"{name}-{version}.bin"
        artifact.write_bytes(data)
        rows.append("\t".join([
            name, version, "exe", artifact.name, hashlib.sha256(data).hexdigest(),
            name + suffix, dependencies.get((name, version), ""),
        ]))
    catalog = directory / "catalog"
    catalog.write_bytes(("\n".join(rows) + "\n").encode("ascii"))
    return catalog


def prepare(case):
    shutil.copytree(Path(__file__).parent / "kit", case.kit, dirs_exist_ok=True)
    catalog = create_catalog(case.root / "catalog")
    case.run("register-sdk", [case.brw, "source", "add", "sdk", str(catalog)])

    # The default is its own Blueprint so applying the sample never selects it.
    bundle = case.root / "default kit"
    bundle.mkdir()
    module = b"""export default () => [
  {op: "tool", registry: "sdk", name: "builder", version: "1"},
  {op: "default", tool: "builder"}
];
"""
    (bundle / "main.mjs").write_bytes(module)
    manifest = (
        "brw-blueprint-v1\nentry\tmain.mjs\nplatform\tany\narch\tany\n"
        "requires\ttool\nrequires\tdefault\n"
        f"module\tmain.mjs\t{hashlib.sha256(module).hexdigest()}\n"
    ).encode("ascii")
    (bundle / "blueprint.manifest").write_bytes(manifest)
    for mode in ("lock", "apply"):
        case.run("default-" + mode, [
            case.brw, "blueprint", mode, str(bundle), "--sha256",
            hashlib.sha256(manifest).hexdigest(), "--workspace", str(case.work), "--json",
        ])
    case.blueprint("lock-sample", "lock")
