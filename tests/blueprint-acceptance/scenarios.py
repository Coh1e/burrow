"""End-to-end checks for the shared Blueprint and its three isolated faults."""

import hashlib
import json
import platform
import zipfile
from pathlib import Path
from unittest import TestCase


equal = TestCase().assertEqual
SYSTEM = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
ARCH = platform.machine().lower()
TARGET = SYSTEM + "-" + {"amd64": "x86_64", "aarch64": "arm64"}.get(ARCH, ARCH)
PLATFORMS = ("windows", "linux", "macos")
MODES = ("Debug", "Release")
BUILD_SITES = [f"main.mjs[{index}] > build/job.mjs[1]" for index in (6, 7)]
SITES = [f"main.mjs[{index}]" for index in range(1, 6)] + BUILD_SITES + ["main.mjs[8]"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def default_entry(case, label):
    path = case.bin / ("builder.exe" if SYSTEM == "windows" else "builder")
    version = case.run(label, [str(path)]).stdout.strip()
    entry = {"path": str(path), "target": str(path.resolve()), "sha256": digest(path), "version": version}
    case.record(label + "-entry", entry)
    equal(version, "1", str(path))
    return entry


def read_only(case, label, mode, expected=0):
    before = case.snapshot()
    report = case.blueprint(label, mode, expected=expected)
    after = case.snapshot()
    case.record(label + "-filesystem", {"before": before, "after": after})
    equal(after, before, label)
    return report


def check_plan(case, report, status, state_status, state_code):
    equal(report["status"], status)
    steps = report["steps"]
    equal([step["source"] for step in steps], SITES)
    equal([step["op"] for step in steps], ["state", "tool", "env", "env", "env", "run", "run", "run"])
    state = steps[0]
    equal((state["status"], state["exitCode"], state["unmet"]), (state_status, state_code, [3]))
    for op, action in (("check", "check"), ("change", "prepare")):
        equal(state[op]["program"], "python")
        equal(state[op]["args"], [{"payload": "project.py"}, action])
    equal((steps[1]["name"], steps[1]["registry"], steps[1]["version"]), ("builder", "sdk", "2"))
    for step, system in zip(steps[2:5], PLATFORMS):
        if system == SYSTEM:
            equal((step["name"], step["value"]), ("BRW_ACCEPT_PLATFORM", system))
            equal(step.get("status"), None)
        else:
            equal((step["status"], step["reason"], step["platform"]), ("skipped", "platform-mismatch", system))
    for step, mode in zip(steps[5:7], MODES):
        equal((step["tool"], step["version"]), ("builder", "2"))
        equal(step["args"], [{"payload": "build/build.py"}, mode, "argument with spaces", {"outputFile": "archive"}])
        equal(step["sha256"], digest(case.kit / "build" / "build.py"))
    equal(steps[7]["program"], "python")
    equal(steps[7]["args"], [{"payload": "project.py"}, "test", {"from": "main.mjs[7]", "output": "archive"}])
    equal([row["name"] for row in report["locked"]], ["base", "middle", "builder"])
    for row, depends in zip(report["locked"], ("", "base", "middle")):
        expected_sha = digest(case.root / "catalog" / (row["name"] + "-2.bin"))
        equal((row["source"], row["version"], row["platform"], row["depends"], row["sha256"]),
              ("sdk", "2", TARGET, depends, expected_sha), row["name"])


def observations(case, label, expected_calls, archives):
    path = case.work / "calls.jsonl"
    calls = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()] if path.exists() else []
    artifacts = {}
    for mode in MODES:
        path = case.work / (mode + ".zip")
        if path.exists():
            with zipfile.ZipFile(path) as archive:
                marker = archive.read("mode.txt").decode("utf-8")
            artifacts[mode] = {"sha256": digest(path), "marker": marker}
    partial = case.work / "Release.partial"
    ready = case.work / "ready"
    facts = {"calls": calls, "archives": artifacts,
             "releasePartial": partial.read_text(encoding="utf-8") if partial.exists() else None,
             "ready": ready.read_text(encoding="utf-8") if ready.exists() else None}
    case.record(label, facts)
    order = [row["mode"] if row["action"] == "build" else row["action"] for row in calls]
    equal(order, expected_calls, label)
    equal({mode: item["marker"] for mode, item in artifacts.items()}, {mode: mode for mode in archives}, label)
    equal(facts["ready"], "ready" if "prepare" in expected_calls else None, label)
    for row in calls:
        equal(Path(row["cwd"]).resolve(), case.work.resolve(), label)
        if row["action"] == "build":
            equal((row["version"], row["platform"]), ("2", SYSTEM), label)
            equal(row["args"][:-1], [row["mode"], "argument with spaces"], label)
            equal(Path(row["args"][-1]).is_absolute(), True, "runtime output file")
            equal(Path(row["script"]).is_absolute(), True, "staged build script")
            equal(row["scriptDigest"], digest(case.kit / "build" / "build.py"), label)
        elif row["action"] == "test":
            equal((row["archive"], row["mode"]), ("Release.zip", "Release"), label)
    return facts


def main(case):
    default = default_entry(case, "initial-default")
    equal(len({case.kit.resolve(), case.launcher.resolve(), case.work.resolve()}), 3)
    preview = read_only(case, "preview", "preview")
    check_plan(case, preview, "planned", "unmet", 3)
    equal(default_entry(case, "preview-default"), default)
    order = ["prepare", "Debug", "Release", "test-start", "test"]
    for label, status, expected in (("apply", "changed", order), ("rerun", "satisfied", order + order[1:])):
        applied = case.blueprint(label, "apply")
        check_plan(case, applied, "applied", status, 0)
        equal(applied["completed"], 6)
        facts = observations(case, label + "-observations", expected, MODES)
        equal(facts["releasePartial"], None)
        equal(default_entry(case, label + "-default"), default)


def missing_lock(case):
    lock = case.kit / f"blueprint.{TARGET}.lock"
    lines = lock.read_bytes().splitlines(keepends=True)
    missing = [line for line in lines if not line.startswith(b"sdk\tbase\t")]
    equal(len(lines) - len(missing), 1)
    lock.write_bytes(b"".join(missing))
    injected = lock.read_bytes()
    default = default_entry(case, "initial-default")
    for mode in ("preview", "apply"):
        report = read_only(case, mode, mode, expected=1)
        equal((report["status"], report["error"], report["detail"], report["completed"]),
              ("failed", "lock-missing", f"base ({TARGET})", 0))
        equal(lock.read_bytes(), injected, "missing dependency restored implicitly")
    observations(case, "after-failure", [], [])
    equal(default_entry(case, "final-default"), default)
    case.record("incomplete-lock", {"file": lock.name, "contents": injected.decode("utf-8"), "sha256": digest(lock)})


def check_error(case):
    fault = case.work / "check-error"
    fault.touch()
    default = default_entry(case, "initial-default")
    for mode in ("preview", "apply"):
        report = read_only(case, mode, mode, expected=1)
        equal((report["status"], report["error"], report["detail"], report["completed"]),
              ("failed", "check-failed", SITES[0], 0))
        equal((report["steps"][0]["exitCode"], report["steps"][0]["unmet"]), (1, [3]))
    observations(case, "after-failure", [], [])
    equal(default_entry(case, "failed-default"), default)
    fault.unlink()
    control = read_only(case, "control-preview", "preview")
    check_plan(case, control, "planned", "unmet", 3)
    control = case.blueprint("control-apply", "apply")
    check_plan(case, control, "applied", "changed", 0)
    observations(case, "after-control", ["prepare", "Debug", "Release", "test-start", "test"], MODES)
    equal(default_entry(case, "final-default"), default)


def release_failure(case):
    fault = case.work / "fail-release"
    fault.touch()
    default = default_entry(case, "initial-default")
    preview = read_only(case, "preview", "preview")
    check_plan(case, preview, "planned", "unmet", 3)
    failed = case.blueprint("apply", "apply", expected=1)
    check_plan(case, failed, "failed", "changed", 0)
    equal((failed["error"], failed["detail"], failed["completed"]), ("child-failed", BUILD_SITES[1], 4))
    equal((failed["commandStarted"], failed["mayHaveSideEffects"]), (True, True))
    order = ["prepare", "Debug", "Release"]
    partial = observations(case, "after-failure", order, ["Debug"])
    equal(partial["releasePartial"], "Release")
    equal(default_entry(case, "failed-default"), default)
    fault.unlink()
    recovered = case.blueprint("repair-apply", "apply")
    check_plan(case, recovered, "applied", "satisfied", 0)
    equal(recovered["completed"], 6)
    observations(case, "after-repair", order + ["Debug", "Release", "test-start", "test"], MODES)
    equal(default_entry(case, "final-default"), default)
