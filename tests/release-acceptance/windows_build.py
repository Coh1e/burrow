"""Build a committed Windows candidate with two offline generators and measure its delivery."""

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile

from windows_delivery import check_delivery, sha256

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/devkit-acceptance"))
from offline import verify_offline


def record(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def run(args):
    revision = subprocess.check_output(["git", "rev-parse", "--verify", args.revision + "^{commit}"],
                                       cwd=ROOT, text=True).strip()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source, evidence = work / "source", work / "evidence"
    source.mkdir()
    evidence.mkdir()
    network = {"before": verify_offline()}
    record(evidence / "network.json", network)
    env = os.environ.copy()
    # CMake's probes and child compilers may only use this fresh SDK and Windows tools.
    system = Path(os.environ["SystemRoot"])
    toolchain = work / "w64devkit"
    tools = toolchain / "bin"
    env["PATH"] = os.pathsep.join(map(str, [tools, system / "System32",
                                            system / "System32/WindowsPowerShell/v1.0", system]))
    for name in ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
                 "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR", "MAKEFLAGS"):
        env.pop(name, None)
    system_tar = system / "System32/tar.exe"

    def logged(file, command, cwd=work):
        print(file, flush=True)
        with tempfile.TemporaryFile() as raw:
            result = subprocess.run(command, env=env, cwd=cwd, stdout=raw,
                                    stderr=subprocess.STDOUT, check=False)
            raw.seek(0)
            message = raw.read().decode("utf-8", errors="replace")
        text = subprocess.list2cmdline([str(arg) for arg in command]) + "\n" + message
        for path, label in ((work, "<work>"), (ROOT, "<repo>")):
            text = text.replace(str(path), label).replace(path.as_posix(), label)
        (evidence / file).write_text(text, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{file}: exit {result.returncode}, see {evidence / file}")
        return message

    source_tar = work / "source.tar"
    subprocess.run(["git", "archive", "--format=tar", f"--output={source_tar}", revision,
                    "LICENSE", "brw", "scripts/blueprint-sources.txt", "tests"], cwd=ROOT, check=True)
    logged("source.log", [system_tar, "-xf", source_tar, "-C", source])
    devkit = args.devkit_archive.resolve()
    expected = (source / "tests/devkit-acceptance/catalog").read_text().splitlines()[1].split("\t")
    if devkit.name != expected[3] or sha256(devkit) != expected[4]:
        raise ValueError("Preseeded devkit differs from the committed catalog")
    logged("devkit.log", [devkit, "-y", "-o."])
    deps = source / ".local/deps/sources"
    deps.mkdir(parents=True)
    pins = {}
    for line in (source / "scripts/blueprint-sources.txt").read_text(encoding="ascii").splitlines():
        filename, expected_hash, _ = line.split()
        path = ROOT / ".local/deps/sources" / filename
        if sha256(path) != expected_hash:
            raise ValueError(f"Preseeded dependency hash mismatch: {filename}")
        pins[filename] = expected_hash
        members = []
        if filename.startswith("libgit2-"):
            members = [filename.split(".tar.")[0] + "/" + part for part in
                       ("CMakeLists.txt", "cmake", "include", "src", "deps", "COPYING")]
        logged("unpack-" + filename + ".log", [system_tar, "-xf", path, "-C", deps, *members])
    versions = {}
    for program in ("gcc", "cmake", "ninja", "mingw32-make", "gdb"):
        versions[program] = logged(program + "-version.log", [tools / (program + ".exe"),
                                                               "--version"]).splitlines()[0]
    results = {"revision": revision, "sourceTarSha256": sha256(source_tar),
               "os": platform.platform(), "machine": platform.machine(),
               "devkitSha256": expected[4], "toolchain": versions, "pinnedSources": pins, "builds": {}}
    record(evidence / "windows-build.json", results)
    for folder, generator, engine in (("ninja", "Ninja", "ninja"), ("mingw-make", "MinGW Makefiles",
                                                                      "mingw32-make")):
        build = work / folder
        logged(folder + "-configure.log", [tools / "cmake.exe", "-S", source / "brw", "-B", build,
               "-G", generator, f"-DCMAKE_MAKE_PROGRAM={(tools / (engine + '.exe')).as_posix()}",
               "-DCMAKE_BUILD_TYPE=Release", "-DBRW_BLUEPRINT=ON", "-DBUILD_TESTING=OFF"])
        logged(folder + "-build.log", [tools / "cmake.exe", "--build", build, "--parallel",
                                       str(args.jobs), "--target", "brw"])
        # Exercise each generator's own binary using isolated, initially empty XDG roots.
        profile = work / (folder + "-profile")
        for key in ("CONFIG", "DATA", "CACHE", "STATE"):
            env["XDG_" + key + "_HOME"] = str(profile / key.lower())
        env["BRW_BIN_DIR"] = str(profile / "bin")
        exe = build / "brw.exe"
        if json.loads(logged(folder + "-list.log", [exe, "list", "--json"])) != {"status": "ok", "packages": []}:
            raise ValueError(f"Unexpected initial package list from {generator}")
        results["builds"][generator] = {"brwSha256": sha256(exe), "bytes": exe.stat().st_size,
                                        "cacheSha256": sha256(build / "CMakeCache.txt")}
        record(evidence / "windows-build.json", results)
    results["delivery"] = check_delivery(work, source, work / "ninja", toolchain, evidence)
    record(evidence / "windows-build.json", results)
    network["after"] = verify_offline()
    record(evidence / "network.json", network)
    record(evidence / "query-build.json", {"revision": revision,
           "brwSha256": results["delivery"]["files"]["brw.exe"]["sha256"],
           "cacheSha256": results["builds"]["Ninja"]["cacheSha256"]})


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--revision", required=True)
    p.add_argument("--work", type=Path, required=True)
    p.add_argument("--devkit-archive", type=Path, required=True)
    p.add_argument("--jobs", type=int, default=4, choices=range(1, 65), metavar="1..64")
    run(p.parse_args())
