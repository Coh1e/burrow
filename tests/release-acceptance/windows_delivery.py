"""Check the Windows delivery bytes, PE imports and DLL loads during native CLI use."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import zipfile


# Inbox components of the Windows 11 x64 baseline used by brw and its OS dependencies.
SYSTEM_DLLS = set(Path(__file__).with_name("windows-system-dlls.txt").read_text("ascii").splitlines())


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pe_imports(path):
    data = path.read_bytes()
    pe, = struct.unpack_from("<I", data, 0x3c)
    machine, count = struct.unpack_from("<HH", data, pe + 4)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    optional = pe + 24
    if (data[pe:pe+4] != b"PE\0\0" or machine != 0x8664 or
            struct.unpack_from("<H", data, optional)[0] != 0x20b):
        raise ValueError(f"{path.name} is not an x64 PE32+ image")
    sections = [struct.unpack_from("<III", data, optional + optional_size + 40*i + 12)
                for i in range(count)]  # VirtualAddress, SizeOfRawData, PointerToRawData

    def offset(rva):
        for start, length, raw in sections:
            if start <= rva < start + length:
                return raw + rva - start
        raise ValueError(f"Unmapped RVA in {path.name}")

    imports = {}
    for name, directory, words, name_index in (("direct", 1, 5, 3), ("delay", 13, 8, 1)):
        rva, size = struct.unpack_from("<II", data, optional + 112 + 8*directory)
        found = set()
        if rva or size:
            start = offset(rva)
            for index in range(size // (4*words)):
                fields = struct.unpack_from("<" + "I"*words, data, start + 4*words*index)
                if not any(fields):
                    break
                if name == "delay" and fields[0] != 1:
                    raise ValueError("Unsupported PE32+ delay import attributes")
                label = offset(fields[name_index])
                found.add(data[label:data.index(b"\0", label)].decode("ascii").lower())
            else:
                raise ValueError(f"Unterminated {name} import table")
        imports[name] = sorted(found)
    if not imports["direct"]:
        raise ValueError(f"No direct imports found in {path.name}")
    return imports


def audit_dlls(names):
    unknown = set(names) - SYSTEM_DLLS
    if unknown:
        raise ValueError(f"Unreviewed DLLs: {sorted(unknown)}")


def runtime_loads(gdb, exe, args, env, cwd, log):
    # Only observe DLL-load events for the brw or generated shim started here.
    command = [gdb, "--nx", "--nh", "--batch", "--return-child-result",
               "-ex", "set debugevents on", "-ex", "run", "--args", exe, *args]
    process = subprocess.run(command, cwd=cwd, env=env, capture_output=True,
                             encoding="utf-8", errors="replace", timeout=120)
    events = process.stderr.splitlines() + process.stdout.splitlines()
    loaded = []
    for event in events:
        if "handle_load_dll: Loading dll" in event:
            module = Path(re.search(r'Loading dll "([^"]+)"', event)[1]).resolve()
            if module.parent != (Path(os.environ["SystemRoot"]) / "System32").resolve():
                raise ValueError(f"Unexpected DLL location for {module.name}")
            loaded.append(module.name.lower())
    log.write_text("\n".join(loaded) + "\n", encoding="utf-8")
    if process.returncode or "ntdll.dll" not in loaded or "kernel32.dll" not in loaded:
        raise ValueError(f"Incomplete DLL trace for {exe.name} {args[0]}: exit {process.returncode}")
    audit_dlls(loaded)
    return sorted(set(loaded))


def write_zip(directory, target):
    # Stable entry dates let an auditor rebuild the download ZIP from the delivered bytes.
    with zipfile.ZipFile(target, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for child in sorted(directory.rglob("*")):
            if child.is_file():
                info = zipfile.ZipInfo(child.relative_to(directory).as_posix())
                archive.writestr(info, child.read_bytes(), zipfile.ZIP_DEFLATED, compresslevel=9)


def check_delivery(work, source, build, toolchain, evidence):
    delivery = work / "delivery"
    delivery.mkdir()
    exe = delivery / "brw.exe"
    shutil.copyfile(build / "brw.exe", exe)
    shutil.copyfile(source / "LICENSE", delivery / "LICENSE")
    licenses = delivery / "licenses"
    shutil.copytree(build / "licenses", licenses)
    deps = source / ".local/deps/sources"
    shutil.copyfile(deps / "zlib-1.3.1/LICENSE", licenses / "zlib.txt")
    shutil.copyfile(deps / "libarchive-3.7.7/COPYING", licenses / "libarchive.txt")
    shutil.copyfile(toolchain / "COPYING.MinGW-w64-runtime.txt", licenses / "mingw-w64-runtime.txt")
    contents = {p.relative_to(delivery).as_posix(): {"bytes": p.stat().st_size, "sha256": sha256(p)}
                for p in sorted(delivery.rglob("*")) if p.is_file()}
    installed = sum(file["bytes"] for file in contents.values())
    if exe.stat().st_size > 8*1024**2 or installed > 9*1024**2:
        raise ValueError("Windows executable or empty installation exceeds its release limit")
    download = work / "brw-windows-x64.zip"
    write_zip(delivery, download)
    with zipfile.ZipFile(download) as archive:
        if set(archive.namelist()) != set(contents):
            raise ValueError("Download ZIP entries differ from the empty installation")
        for name, expected in contents.items():
            with archive.open(name) as member:
                if hashlib.file_digest(member, "sha256").hexdigest() != expected["sha256"]:
                    raise ValueError(f"Download ZIP digest mismatch: {name}")

    profile = work / "delivery-profile"
    profile.mkdir()
    env = {**os.environ, "USERPROFILE": str(profile), "BRW_BIN_DIR": str(profile / "bin")}
    for key in ("CONFIG", "DATA", "STATE", "CACHE"):
        env[f"XDG_{key}_HOME"] = str(profile / key.lower())
    windows = Path(os.environ["SystemRoot"])
    env["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    payload = work / "payload.tar"
    with tarfile.open(payload, "x", format=tarfile.USTAR_FORMAT) as archive:
        archive.add(exe, arcname="payload.exe")
    gdb = toolchain / "bin/gdb.exe"
    direct = {}
    for name, binary in (("brw", exe), ("console-shim", build / "brw_shim.exe"),
                         ("gui-shim", build / "brw_gui_shim.exe")):
        imports = pe_imports(binary)
        for names in imports.values():
            audit_dlls(names)
        direct[name] = imports
    workloads = [("empty-list", exe, ["list", "--json"]),
                 ("install", exe, ["install", "candidate", "1", str(payload), sha256(payload),
                                  "payload.exe", "--json"])]
    kit = work / "dll-trace-blueprint"
    kit.mkdir()
    module = kit / "main.mjs"
    module.write_bytes(b'export default () => [{op:"env",name:"BRW_RELEASE_OBSERVATION",value:"ok"}];\n')
    manifest = kit / "blueprint.manifest"
    manifest.write_bytes(("brw-blueprint-v1\nentry\tmain.mjs\nplatform\tany\narch\tany\nrequires\tenv\n"
                          f"module\tmain.mjs\t{sha256(module)}\n").encode("ascii"))
    workloads.append(("quickjs-apply", exe, ["blueprint", "apply", str(kit), "--sha256",
                                              sha256(manifest), "--workspace", str(profile), "--json"]))
    shim = profile / "bin/candidate.exe"
    workloads.append(("command-entry", shim, ["list", "--json"]))
    loads = {}
    for name, program, args in workloads:
        loads[name] = runtime_loads(gdb, program, args, env, work, evidence / (name + "-dlls.txt"))
    if sha256(shim) != sha256(build / "brw_shim.exe"):
        raise ValueError("The generated command entry differs from the embedded console shim")
    return {"files": contents, "emptyInstallBytes": installed,
            "download": {"bytes": download.stat().st_size, "sha256": sha256(download)},
            "commandEntry": {"bytes": shim.stat().st_size, "sha256": sha256(shim)},
            "peImports": direct, "runtimeLoads": loads}
