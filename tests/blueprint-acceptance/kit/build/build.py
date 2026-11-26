"""The verified child payload records exactly what the locked builder ran."""

import hashlib
import json
import os
import sys
import zipfile
from pathlib import Path


mode, label, output_file = sys.argv[1:]
script = Path(__file__).resolve()
with Path("calls.jsonl").open("a", encoding="utf-8") as calls:
    calls.write(json.dumps({
        "action": "build",
        "mode": mode,
        "args": sys.argv[1:],
        "cwd": os.getcwd(),
        "version": os.environ["BRW_ACCEPT_BUILDER"],
        "platform": os.environ["BRW_ACCEPT_PLATFORM"],
        "script": str(script),
        "scriptDigest": hashlib.sha256(script.read_bytes()).hexdigest(),
    }) + "\n")

if mode == "Release" and Path("fail-release").exists():
    Path("Release.partial").write_text(mode, encoding="utf-8")
    sys.exit(17)

archive = Path(mode + ".zip")
with zipfile.ZipFile(archive, "w") as package:
    package.writestr("mode.txt", mode)
Path(output_file).write_bytes((archive.name + "\n").encode("utf-8"))
