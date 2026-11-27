"""Measure native Windows package queries, including process startup and output capture."""

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import time
import winreg


ROOT = Path(__file__).resolve().parents[2]
REPEATS = 20


def measure(brw, env, args, expected_names):
    # First invocation of this workload is separate. Each timing uses a new process.
    samples = []
    for _ in range(1 + REPEATS):
        start = time.perf_counter_ns()
        result = subprocess.run([brw, *args, "--json"], env=env, capture_output=True, timeout=60)
        samples.append((time.perf_counter_ns() - start) / 1_000_000)
        value = json.loads(result.stdout)
        if args[0] == "show" and not expected_names:
            valid = result.returncode == 1 and value.get("error") == "package-not-found"
        else:
            packages = [value["package"]] if args[0] == "show" else value["packages"]
            valid = result.returncode == 0 and sorted(pkg["name"] for pkg in packages) == expected_names
        if not valid:
            raise RuntimeError(f"{args} returned unexpected CLI results (exit {result.returncode})")
    first, *repeated = samples
    ordered = sorted(repeated)
    return {"command": ["brw", *args, "--json"], "matches": len(expected_names),
            "firstMs": first, "repeatMs": repeated,
            "p50Ms": ordered[math.ceil(0.50 * len(ordered)) - 1],
            "p95Ms": ordered[math.ceil(0.95 * len(ordered)) - 1]}


def fixture(brw, base, count):
    base.mkdir()
    env = os.environ.copy()
    env["USERPROFILE"] = str(base)
    for kind in ("CONFIG", "STATE", "DATA", "CACHE"):
        env[f"XDG_{kind}_HOME"] = str(base / kind.lower())
    env["BRW_BIN_DIR"] = str(base / "bin")
    payload = base / "probe.cmd"
    payload.write_bytes(b"@exit /b 0\r\n")
    checksum = hashlib.sha256(payload.read_bytes()).hexdigest()
    names = [f"pkg{i:05d}" for i in range(count)]
    catalog = base / "catalog.tsv"
    with catalog.open("w", encoding="ascii", newline="\n") as stream:
        stream.write("brw-catalog-v1\n")
        for name in names:
            stream.write(f"{name}\t1\texe\tprobe.cmd\t{checksum}\tprobe.cmd\n")
    subprocess.run([brw, "source", "add", "bench", catalog], env=env, capture_output=True, check=True)
    return env, names


def machine(work):
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
        cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    fs = ctypes.create_unicode_buffer(64)
    if not ctypes.windll.kernel32.GetVolumeInformationW(work.anchor, None, 0, None, None, None, fs, len(fs)):
        raise ctypes.WinError()
    return {"os": platform.platform(), "architecture": platform.machine(), "cpu": cpu,
            "fixtureDrive": work.anchor, "fileSystem": fs.value,
            "driveTypeCode": ctypes.windll.kernel32.GetDriveTypeW(work.anchor)}


def run(brw, cache, build_record, evidence, storage):
    if evidence.exists():
        raise FileExistsError(evidence)
    recorded = json.loads(build_record.read_bytes())
    revision = recorded["revision"]
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Expected a full candidate commit in the delivery build record")
    subprocess.run(["git", "cat-file", "-e", revision + "^{commit}"], cwd=ROOT, check=True)
    binary_sha = hashlib.sha256(brw.read_bytes()).hexdigest()
    cache_sha = hashlib.sha256(cache.read_bytes()).hexdigest()
    if binary_sha != recorded["brwSha256"] or cache_sha != recorded["cacheSha256"]:
        raise ValueError("The binary or CMake cache differs from the candidate delivery build record")
    options = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        key = line.partition(":")[0]
        if key in {"CMAKE_BUILD_TYPE", "CMAKE_GENERATOR", "CMAKE_C_FLAGS", "BRW_BLUEPRINT",
                   "CMAKE_C_FLAGS_RELEASE", "CMAKE_C_FLAGS_MINSIZEREL", "BRW_ENABLE_CPP_MODULE",
                   "BUILD_TESTING", "CMAKE_EXE_LINKER_FLAGS"}:
            options[key] = line.partition("=")[2]
    evidence.parent.mkdir(parents=True, exist_ok=True)
    result = {"revision": revision, "brwSha256": binary_sha, "cacheSha256": cache_sha, "build": options,
              "host": machine(evidence.parent), "repeats": REPEATS, "percentiles": "nearest rank",
              "cache": "Catalogs freshly written and registered. Package installations use the CLI. "
                       "No OS cache eviction. First invocation per query, then consecutive fresh processes.",
              "cases": []}
    if not storage or not storage.strip():
        raise ValueError("Supply the model and bus of the disk holding the workload")
    result["host"]["storage"] = storage.strip()
    with tempfile.TemporaryDirectory(dir=evidence.parent) as scratch:
        work = Path(scratch)
        for count in (32, 512, 2048):
            env, names = fixture(brw, work / f"catalog-{count}", count)
            for verb in ("search", "show"):
                for query, expected in [(names[-1], [names[-1]]), ("absent", [])]:
                    measured = measure(brw, env, [verb, query], expected)
                    result["cases"].append(dict(catalogRows=count, installed=0, **measured))
        env, names = fixture(brw, work / "installed", 64)
        installed = []
        for count in (0, 8, 64):
            for name in names[len(installed):count]:
                subprocess.run([brw, "install", name], env=env, check=True, capture_output=True)
                installed.append(name)
            measured = measure(brw, env, ["list"], installed)
            result["cases"].append(dict(catalogRows=len(names), installed=count, **measured))
    with evidence.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--brw", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True, help="CMakeCache.txt for this binary")
    parser.add_argument("--build-record", type=Path, required=True, help="query-build.json from the delivery build")
    parser.add_argument("--evidence", type=Path, required=True, help="new output JSON")
    parser.add_argument("--storage", required=True, help="model and bus of the disk holding the evidence directory")
    args = parser.parse_args()
    run(args.brw.resolve(strict=True), args.cache.resolve(strict=True), args.build_record.resolve(strict=True),
        args.evidence.resolve(), args.storage)
