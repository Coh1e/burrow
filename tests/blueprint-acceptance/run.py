"""Run the Blueprint acceptance checklist against a native brw build."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import traceback

sys.dont_write_bytecode = True
import fixture
import scenarios


REPO = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


class Case:
    def __init__(self, root, evidence, brw, env):
        self.root = root
        self.evidence = evidence
        self.brw = str(brw)
        self.work = root / "project with spaces"
        self.kit = root / "kit"
        self.launcher = root / "launcher"
        self.bin = root / "bin"
        self.env = env.copy()
        for path in (self.work, self.launcher, self.evidence):
            path.mkdir(parents=True)
        for kind in ("CONFIG", "CACHE", "DATA", "STATE"):
            self.env[f"XDG_{kind}_HOME"] = str(root / kind.lower())
        self.env["BRW_BIN_DIR"] = str(self.bin)

    def run(self, label, argv, expected=0):
        command = [str(arg) for arg in argv]
        result = subprocess.run(command, cwd=self.launcher, env=self.env, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=300)
        with (self.evidence / "commands.jsonl").open("a", encoding="utf-8") as output:
            output.write(json.dumps(dict(label=label, cwd=str(self.launcher), argv=command,
                                         exit=result.returncode, stdout=result.stdout,
                                         stderr=result.stderr), ensure_ascii=False) + "\n")
        if result.returncode != expected:
            raise AssertionError(f"{label}: expected exit {expected}, got {result.returncode}: "
                                 f"{result.stderr[-500:]} {result.stdout[-800:]}")
        return result

    def blueprint(self, label, mode, expected=0):
        manifest = self.kit / "blueprint.manifest"
        result = self.run(label, [self.brw, "blueprint", mode, self.kit, "--sha256",
                                  digest(manifest), "--workspace", self.work, "--json"], expected)
        return json.loads(result.stdout)

    def record(self, name, value):
        write_json(self.evidence / (name + ".json"), value)

    def snapshot(self):
        roots = {"workspace": self.work, "bin": self.bin}
        roots.update({kind: Path(self.env[f"XDG_{kind}_HOME"]) for kind in
                      ("DATA", "CONFIG", "CACHE", "STATE")})
        result = {}
        for name, path in roots.items():
            files = {}
            for entry in sorted(path.rglob("*")):
                relative = entry.relative_to(path).as_posix()
                if entry.is_symlink():
                    files[relative] = {"link": os.readlink(entry)}
                elif entry.is_file():
                    files[relative] = digest(entry)
                else:
                    files[relative] = "directory"
            result[name] = files if path.exists() else None
        return result


def command_text(*argv):
    return subprocess.check_output(argv, cwd=REPO, text=True).strip()


def run(build, evidence, scratch):
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith(("#", "//")):
            key, value = line.split("=", 1)
            cache[key.split(":")[0]] = value
    cmake = Path(cache["CMAKE_COMMAND"])
    compiler = Path(cache["CMAKE_C_COMPILER"])
    suffix = ".exe" if os.name == "nt" else ""
    brw = build / ("brw" + suffix)
    env = os.environ.copy()
    python_dir = Path(sys.executable).parent
    if not (python_dir / ("python" + suffix)).is_file():
        python_dir = scratch / "python-bin"
        python_dir.mkdir()
        (python_dir / "python").symlink_to(sys.executable)
    env["PATH"] = os.pathsep.join(map(str, (python_dir, cmake.parent, compiler.parent, env["PATH"])))
    temp = scratch / "tmp"
    temp.mkdir()
    env.update(TMPDIR=str(temp), TMP=str(temp), TEMP=str(temp), PYTHONDONTWRITEBYTECODE="1")
    metadata = dict(system=platform.system(), os=platform.platform(), machine=platform.machine(),
                    python=sys.version.split()[0], build=str(build), buildType=cache["CMAKE_BUILD_TYPE"],
                    compiler=command_text(compiler, "--version").splitlines()[0],
                    cmake=command_text(cmake, "--version").splitlines()[0],
                    revision=command_text("git", "rev-parse", "HEAD"),
                    brwTree=command_text("git", "rev-parse", "HEAD:brw"),
                    worktreeChanges=command_text("git", "status", "--short"),
                    executableSha256=digest(brw))
    if sys.platform == "linux":
        release = Path("/etc/os-release")
        if release.is_file():
            metadata["distribution"] = next((line for line in release.read_text().splitlines()
                                             if line.startswith("PRETTY_NAME=")), "")
    summary = {"host": metadata, "results": {}}
    for test in (scenarios.main, scenarios.missing_lock, scenarios.check_error, scenarios.release_failure):
        name = test.__name__
        case = Case(scratch / name, evidence / name, brw, env)
        try:
            fixture.prepare(case)
            case.record("manifest", (case.kit / "blueprint.manifest").read_text(encoding="utf-8"))
            lock, = case.kit.glob("blueprint.*.lock")
            case.record("initial-lock", {"file": lock.name, "sha256": digest(lock),
                                         "contents": lock.read_text(encoding="utf-8")})
            summary.setdefault("manifestSha256", digest(case.kit / "blueprint.manifest"))
            case.record("initial-state", case.snapshot())
            test(case)
            outcome = "passed"
        except Exception:
            outcome = traceback.format_exc()
        case.record("final-state", case.snapshot())
        summary["results"][name] = outcome
        print(f"{name}: {outcome}", flush=True)
    boundary = Case(scratch / "boundary", evidence / "boundary", brw, env)
    boundary.env["BRW_BLUEPRINT_EVIDENCE"] = str(boundary.evidence)
    try:
        boundary.run("boundary", [cmake.with_name("ctest" + suffix), "--test-dir", build, "-R",
                                  "^blueprint_cli$", "--no-tests=error", "--output-on-failure"])
        groups = (boundary.evidence / "passed.txt").read_text(encoding="utf-8").splitlines()
        if {line.split("/")[0] for line in groups} != set("1234567"):
            raise AssertionError(f"Incomplete boundary evidence: {groups}")
        outcome = "passed"
    except Exception:
        outcome = traceback.format_exc()
    summary["results"]["boundary"] = outcome
    print(f"boundary: {outcome}", flush=True)
    write_json(evidence / "summary.json", summary)
    return all(value == "passed" for value in summary["results"].values())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    evidence_dir = args.evidence.resolve()
    evidence_dir.mkdir(parents=True, exist_ok=True)
    if any(evidence_dir.iterdir()):
        parser.error("--evidence must be empty or new")
    scratch_parent = REPO / ".local"
    scratch_parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bp144-", dir=scratch_parent) as directory:
        sys.exit(0 if run(args.build_dir.resolve(), evidence_dir, Path(directory)) else 1)
