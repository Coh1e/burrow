"""Shared process and workspace assertions for the CLI and Blueprint archive fixtures."""

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess


def sha(data):
    return hashlib.sha256(data).hexdigest()


def snapshot(root, include_times):
    result = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            info = path.lstat()
            assert not stat.S_ISLNK(info.st_mode), path
            assert stat.S_ISDIR(info.st_mode) or stat.S_ISREG(info.st_mode), path
            contents = path.read_bytes() if stat.S_ISREG(info.st_mode) else None
            result[path.relative_to(root)] = (info.st_mode, contents, info.st_mtime_ns if include_times else None)
    return result


def invoke(args, root, expected_code):
    env = os.environ.copy()
    for variable in ("XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "BRW_BIN_DIR"):
        env[variable] = str(root / variable)
    for variable in ("TMP", "TEMP", "TMPDIR"):
        env[variable] = str(root / "temp")
    run = subprocess.run(args, cwd=root, env=env, capture_output=True, timeout=25)
    assert run.returncode == expected_code, (args, run.returncode, run.stdout, run.stderr)
    return json.loads(run.stdout)


def blueprint_args(brw, bundle, digest, work, mode):
    return [brw, "blueprint", "preview" if mode == "preview" else "apply", bundle,
            "--sha256", digest, "--workspace", work, "--json"] + (["--dry-run"] if mode == "dry-run" else [])
