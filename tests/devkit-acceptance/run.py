"""Exercise the Windows devkit Blueprint on an offline host with a preseeded archive."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from offline import verify_offline


HERE = Path(__file__).resolve().parent
KIT = HERE / "kit"
ARCHIVE = "w64devkit-x64-2.11.0.7z.exe"
MANIFEST = "blueprint.manifest"
TRUSTED_MANIFEST = "358a93fcdeb87b4a2d0a279295194be25e0ef848b9af5951c32be9a69f772593"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def directory_state(roots):
    return {str(path): {child.relative_to(path).as_posix(): sha256(child) if child.is_file() else None
                        for child in sorted(path.rglob("*"))} if path.exists() else None
            for path in roots}


def check_rejections(run_blueprint, root, workspace):
    results = {}
    variants = {"tampered": "file-digest-mismatch", "missing": "bundle-file-unavailable",
                "capability": "missing-capability", "oversized": "bundle-limit"}
    for name, error in variants.items():
        kit = root / "invalid" / name
        shutil.copytree(KIT, kit)
        module = kit / "session.mjs"
        if name == "tampered":
            with module.open("ab") as out:
                out.write(b"\n// modified\n")
        elif name == "missing":
            module.unlink()
        elif name == "capability":
            with (kit / MANIFEST).open("ab") as out:
                out.write(b"requires\tunknown-devkit-capability\n")
        else:
            module.write_bytes(b" " * (1024 * 1024 + 1))
        # For the capability variant the operator trusts this different manifest.
        for mode in ("preview", "apply"):
            trusted = sha256(kit / MANIFEST) if name == "capability" else TRUSTED_MANIFEST
            result = run_blueprint(name + "-" + mode, mode, kit, 1, trusted)
            detail = "unknown-devkit-capability" if name == "capability" else module.name
            require((result.get("error"), result.get("detail"), result.get("completed")) == (error, detail, 0),
                    f"{name} {mode}: expected {error} for {detail} with zero completed steps: {result}")
            require(not any(workspace.iterdir()), f"{name} {mode} left files in the workspace")
        results[name] = error
    sentinel = workspace / "keep.txt"
    sentinel.write_bytes(b"existing user file")
    for mode in ("preview", "apply"):
        result = run_blueprint("occupied-" + mode, mode, KIT, 1)
        require((result.get("error"), result.get("detail"), result.get("completed")) ==
                ("host-requirement-unmet", "main.mjs[1] > toolchain.mjs[1]", 0),
                f"{mode} did not refuse the occupied workspace before extraction")
        require(list(workspace.iterdir()) == [sentinel] and sentinel.read_bytes() == b"existing user file",
                f"{mode} modified the occupied workspace")
    sentinel.unlink()
    results["occupied"] = "host-requirement-unmet"
    return results


def run(brw, archive, root, report):
    version = sys.getwindowsversion()
    require(version.product_type == 1 and version.build >= 22000 and platform.machine().lower() == "amd64",
            "Windows 11 x64 is required")
    report.update(host=platform.platform(), architecture=platform.machine(), invocation=sys.orig_argv,
                  network=verify_offline(), brw=str(brw), brwSha256=sha256(brw),
                  manifestSha256=sha256(KIT / MANIFEST), lockSha256=sha256(KIT / "blueprint.windows-x86_64.lock"),
                  archive=str(archive), archiveSha256=sha256(archive))
    require(report["manifestSha256"] == TRUSTED_MANIFEST, "Unexpected devkit Blueprint manifest")
    catalog = HERE / "catalog"
    require(report["archiveSha256"] == catalog.read_text().splitlines()[1].split("\t")[4],
            "The supplied archive does not match the pinned catalog digest")
    source, caller, initial, moved = (root / name for name in
                                      ("source", "caller", "initial", "relocated devkit"))
    for directory in (source, caller, initial, root / "tmp"):
        directory.mkdir()
    shutil.copyfile(archive, source / ARCHIVE)
    shutil.copyfile(catalog, source / "catalog")
    shutil.copytree(HERE / "probe", caller / "probe")
    system = Path(os.environ["SystemRoot"]) / "System32"
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("CMAKE_", "BRW_", "XDG_", "GCC_", "MSYS")) or key in {
            "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "CPATH", "COMPILER_PATH", "LIBRARY_PATH",
            "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "MAKEFLAGS", "W64DEVKIT_HOME"
        }:
            del env[key]
    env.update(PATH=os.pathsep.join(map(str, (system, system.parent, system / "WindowsPowerShell/v1.0"))),
               TMP=str(root / "tmp"), TEMP=str(root / "tmp"), BRW_BIN_DIR=str(root / "entries"))
    for kind in ("CACHE", "CONFIG", "DATA", "STATE"):
        env[f"XDG_{kind}_HOME"] = str(root / kind.lower())
    report.update(initial=str(initial), relocated=str(moved), caller=str(caller), parentPath=env["PATH"])

    def invoke(name, args, expected=0, input_text=None):
        process = subprocess.run(args, cwd=caller, env=env, input=input_text,
                                 stdin=subprocess.DEVNULL if input_text is None else None, capture_output=True,
                                 text=True, encoding="utf-8", errors="replace", timeout=300)
        with (root / "commands.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(dict(label=name, command=args, cwd=str(caller), exit=process.returncode,
                                      stdin=input_text, stdout=process.stdout, stderr=process.stderr)) + "\n")
        require(process.returncode == expected,
                f"{name} returned {process.returncode}, expected {expected}: "
                f"{process.stdout[-600:]} {process.stderr[-600:]}")
        return process.stdout

    def blueprint(label, mode, kit=KIT, expected=0, trusted=TRUSTED_MANIFEST):
        return json.loads(invoke(label, [str(brw), "blueprint", mode, str(kit), "--sha256",
                                         trusted, "--workspace", str(initial), "--json"], expected))

    invoke("register-offline-source", [str(brw), "source", "add", "devkit", str(source / "catalog")])
    observed = [initial, root / "entries", *(root / name for name in ("cache", "config", "data", "state"))]
    before = directory_state(observed)
    report["rejections"] = check_rejections(blueprint, root, initial)
    preview = blueprint("preview", "preview")
    after = directory_state(observed)
    report["planningSnapshots"] = {"before": before, "after": after}
    require(after == before, "Filesystem snapshots differ after the rejected cases and successful preview")
    apply = blueprint("apply", "apply")
    steps = [(s["op"], s["source"]) for s in preview["steps"]]
    require(steps == [(s["op"], s["source"]) for s in apply["steps"]], "Apply differs from preview")
    require(any("toolchain.mjs" in site and op == "run" for op, site in steps) and
            any("session.mjs" in site and op == "copy" for op, site in steps), f"Missing child call: {steps}")
    require(preview["status"] == "planned" and preview["completed"] == 0 and
            apply["status"] == "applied" and apply["completed"] == len(steps), "Incomplete Blueprint")
    report["steps"] = steps
    # Both endpoints are direct children of the newly created, caller-selected run directory.
    require(initial.resolve().parent == root and moved.resolve().parent == root and not moved.exists(),
            "Invalid relocation destination")
    initial.rename(moved)
    require(not initial.exists() and (moved / "devkit.cmd").is_file(), "Incomplete devkit relocation")

    env["BRW_ACCEPTANCE_ENTRY"] = str(moved / "devkit.cmd")
    # Simulate a caller still configured for the old installation.
    for key in ("GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH"):
        env[key] = str(initial / "w64devkit") + os.sep

    def session(label, command="", input_text=None):
        # Launch cmd's fixed command text from caller. A quoted variable supplies the entry path.
        return invoke(label, f'"{system / "cmd.exe"}" /d /v:off /s /c ""%BRW_ACCEPTANCE_ENTRY%" {command}"',
                      input_text=input_text)

    bin_dir = (moved / "w64devkit/bin").resolve(strict=True)
    require(bin_dir.is_relative_to(moved), f"Tool directory resolves outside the relocated devkit: {bin_dir}")
    shell = session("interactive-session", input_text="@echo off\necho SESSION_CWD=%CD%\nwhere.exe gcc.exe\nexit 0\n")
    lines = shell.splitlines()
    require("SESSION_CWD=" + str(caller) in lines and
            any(Path(line) == bin_dir / "gcc.exe" for line in lines),
            "Interactive session did not retain the caller directory and relocated compiler")
    session("configure", 'cmake -S probe -B build -G "MinGW Makefiles"')
    build = caller / "build"
    cache = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line:
            key, value = line.split("=", 1)
            cache[key.split(":")[0]] = value
    tools = {}
    for key in ("CMAKE_COMMAND", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"):
        path = Path(cache[key]).resolve(strict=True)
        require(path.parent == bin_dir, f"{key} selected {path}, outside {bin_dir}")
        tools[key] = str(path)
    for compiler, frontend in (("gcc", "cc1"), ("g++", "cc1plus")):
        selected = session("find-" + frontend, f"{compiler} -print-prog-name={frontend}").strip()
        path = Path(selected).resolve(strict=True)
        require(path.is_relative_to(moved) and path.name.lower() == frontend + ".exe",
                f"{compiler} selected an unexpected frontend: {path}")
        tools[frontend] = str(path)
    report["tools"] = tools
    report["compileCommands"] = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    report["targetTriple"] = session("compiler-target", "gcc -dumpmachine").strip()
    require(report["targetTriple"] == "x86_64-w64-mingw32", "Expected a MinGW-w64 x64 target")
    report["versions"] = {
        "gcc": session("gcc-version", "gcc -dumpfullversion").strip(),
        "g++": session("g++-version", "g++ -dumpfullversion").strip(),
        "cmake": session("cmake-version", "cmake --version").splitlines()[0],
    }
    report["programs"] = {}
    for target, output in (("c_probe", "C: 42"), ("cxx_probe", "C++: 42")):
        session("build-" + target, f"cmake --build build --target {target} --verbose")
        actual = session("run-" + target, f"build\\{target}.exe").strip()
        require(actual == output, f"{target}: expected {output!r}, got {actual!r}")
        report["programs"][target] = {"stdout": actual, "sha256": sha256(build / (target + ".exe"))}
    report["networkAfterBuild"] = verify_offline()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--brw", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--run-dir", type=Path, required=True, help="a new directory for files and evidence")
    args = parser.parse_args()
    root = args.run_dir.resolve()
    root.mkdir(parents=True, exist_ok=False)
    report = {}
    try:
        run(args.brw.resolve(strict=True), args.archive.resolve(strict=True), root, report)
        report["status"] = "passed"
    except Exception as exc:
        report.update(status="failed", error=str(exc))
        raise
    finally:
        (root / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(root / "summary.json")
