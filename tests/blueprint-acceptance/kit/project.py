"""Read-only state check, corresponding change, and consumer of the second archive."""

import json
import os
import sys
import zipfile
from pathlib import Path


def record(event, **fields):
    with Path("calls.jsonl").open("a", encoding="utf-8") as calls:
        calls.write(json.dumps({"action": event, "cwd": os.getcwd(), **fields}) + "\n")


action = sys.argv[1]
ready = Path("ready")
if action == "check":
    if Path("check-error").exists():
        sys.exit(1)
    sys.exit(0 if ready.is_file() else 3)

if action == "prepare":
    ready.write_text("ready", encoding="utf-8")
    record("prepare")
elif action == "test":
    record("test-start")
    archive = sys.argv[2]
    with zipfile.ZipFile(archive) as package:
        mode = package.read("mode.txt").decode("utf-8")
    record("test", archive=archive, mode=mode)
    if mode != "Release":
        sys.exit(4)
else:
    sys.exit(2)
