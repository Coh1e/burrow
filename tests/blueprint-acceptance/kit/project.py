"""Read-only state check, corresponding change, and consumer of the second archive."""

import json
import os
import sys
import zipfile
from pathlib import Path


action = sys.argv[1]
ready = Path("ready")
if action == "check":
    if Path("check-error").exists():
        sys.exit(1)
    sys.exit(0 if ready.is_file() else 3)

record = {"action": action, "cwd": os.getcwd()}
if action == "prepare":
    ready.write_text("ready", encoding="utf-8")
elif action == "test":
    archive = sys.argv[2]
    with zipfile.ZipFile(archive) as package:
        mode = package.read("mode.txt").decode("utf-8")
    record.update(archive=archive, mode=mode)
else:
    sys.exit(2)

with Path("calls.jsonl").open("a", encoding="utf-8") as calls:
    calls.write(json.dumps(record) + "\n")
if action == "test" and mode != "Release":
    sys.exit(4)
