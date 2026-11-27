"""Exercise standalone and nested Blueprint workspace extraction through brw."""

import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile


MEMBERS = [("nested/input.txt", b"read TOKEN\n"), ("last.txt", b"last complete file\n")]
OUTPUT = "products/new tree"
CALL = "main.mjs[1] > children/extract.mjs[1]"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def archive_bytes(kind):
    out = io.BytesIO()
    if kind.startswith("zip"):
        compression = zipfile.ZIP_STORED if kind == "zip-bad-crc" else zipfile.ZIP_DEFLATED
        with zipfile.ZipFile(out, "w", compression=compression) as archive:
            if kind != "zip-empty":
                for name, data in MEMBERS:
                    archive.writestr(name, data)
        data = out.getvalue()
        if kind == "zip-bad-crc":
            assert data.count(MEMBERS[-1][1]) == 1
            return data.replace(MEMBERS[-1][1], b"X" + MEMBERS[-1][1][1:])
        if kind == "zip-truncated":
            return data[:data.index(b"PK\x01\x02")]  # Keep both complete members, omit the directory.
        return data
    with tarfile.open(fileobj=out, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in MEMBERS:
            # Even if erroneously accepted, the final entry stays inside this fixture.
            member = tarfile.TarInfo("../unexpected" if kind == "outside" and name == "last.txt" else name)
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    data = out.getvalue()
    if kind == "damaged":
        with tarfile.open(fileobj=io.BytesIO(data)) as archive:
            last = archive.getmember("last.txt")
        return data[:last.offset_data + 1]  # The last header is intact, its body is incomplete.
    return gzip.compress(data) if kind == "gzip" else data


def make_bundle(bundle, data, steps):
    child = bundle / "children"
    child.mkdir(parents=True)
    (bundle / "main.mjs").write_text(
        'export default () => [{op:"include",module:"./children/extract.mjs",args:{}}];', encoding="ascii")
    (child / "extract.mjs").write_text("export default () => " + json.dumps(steps), encoding="ascii")
    (child / "data.archive").write_bytes(data)
    (child / "marker.txt").write_bytes(b"finished")
    rows = ["brw-blueprint-v1", "entry\tmain.mjs", "platform\tany", "arch\tany",
            "requires\tunpack", "requires\treplace", "requires\tcopy"]
    for name in ("main.mjs", "children/extract.mjs", "children/data.archive", "children/marker.txt"):
        kind = "module" if name.endswith(".mjs") else "payload"
        rows.append(f"{kind}\t{name}\t{sha((bundle / name).read_bytes())}")
    manifest = ("\n".join(rows) + "\n").encode("ascii")
    (bundle / "blueprint.manifest").write_bytes(manifest)
    return sha(manifest)


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


def unpack(path):
    return {"op": "unpack", "payload": "./data.archive", "path": path}


def replace_from_archive(path):
    return {"op": "replace", "path": path + "/nested/input.txt", "from": "TOKEN", "to": "result", "count": 1}


def marker(path):
    return {"op": "copy", "payload": "./marker.txt", "path": path}


def blueprint_args(brw, bundle, digest, work, mode):
    return [brw, "blueprint", "preview" if mode == "preview" else "apply", bundle,
            "--sha256", digest, "--workspace", work, "--json"] + (["--dry-run"] if mode == "dry-run" else [])


def exercise(root, brw, kind, blueprint):
    root.mkdir()
    (root / "temp").mkdir()
    work = root / "workspace"
    products = work / "products"
    products.mkdir(parents=True)
    (products / "retained.txt").write_bytes(b"keep")
    destination = work / OUTPUT
    if kind in ("existing", "empty"):
        destination.mkdir()
        if kind == "existing":
            (destination / "prior.txt").write_bytes(b"prior contents")
    data = archive_bytes(kind)
    bundle = root / "kit"
    empty_archive = kind == "zip-empty"
    steps = [unpack(OUTPUT)]
    if not empty_archive:
        steps.append(replace_from_archive(OUTPUT))
    steps.append(marker("completed.txt"))
    manifest = make_bundle(bundle, data, steps)
    payload = bundle / "children/data.archive"
    if kind == "digest":
        payload.write_bytes(data + b"x")
    error = {"outside": "archive-path", "damaged": "archive-data", "digest": "file-digest-mismatch",
             "zip-bad-crc": "archive-data", "zip-truncated": "archive-data",
             "empty": "destination-exists", "existing": "destination-exists"}.get(kind)
    modes = ("preview", "dry-run", "apply") if blueprint else ("dry-run", "apply")
    for mode in modes:
        preview = mode != "apply"
        if blueprint:
            args = blueprint_args(brw, bundle, manifest, work, mode)
        else:
            args = [brw, "unpack", payload, OUTPUT, "--sha256", sha(data), "--workspace", work, "--json"]
            if preview:
                args.append("--dry-run")
        before = snapshot(root, preview)
        report = invoke(args, root, 1 if error else 0)
        if error:
            assert (report["status"], report["error"]) == ("failed", error), report
            if blueprint and kind != "digest":
                assert report["detail"] == CALL and report["completed"] == 0, report
        else:
            assert report["status"] == ("planned" if preview else "applied"), report
            if blueprint:
                step = report["steps"][0]
                assert (step["op"], step["source"], step["payload"]) == ("unpack", CALL, "children/data.archive"), step
                ops = [step["op"] for step in report["steps"]]
                assert ops == (["unpack", "copy"] if empty_archive else ["unpack", "replace", "copy"]), report
                assert report["completed"] == (0 if preview else len(ops)), report
            else:
                step = report
                assert step["operation"] == "unpack", report
            assert (step["path"], step["sha256"]) == (OUTPUT, sha(data)), report
        if preview or error:
            after = snapshot(root, preview)
            assert after == before, (kind, blueprint, mode, before.keys() ^ after.keys())
        else:
            if empty_archive:
                assert list(destination.iterdir()) == []
            else:
                expected_text = b"read result\n" if blueprint else MEMBERS[0][1]
                assert (destination / "nested/input.txt").read_bytes() == expected_text
                assert (destination / "last.txt").read_bytes() == MEMBERS[1][1]
            assert (products / "retained.txt").read_bytes() == b"keep"
            assert {p.name for p in products.iterdir()} == {"retained.txt", destination.name}
            if blueprint:
                assert (work / "completed.txt").read_bytes() == b"finished"


def exercise_composition(root, brw, scenario):
    root.mkdir()
    (root / "temp").mkdir()
    work = root / "workspace"
    (work / "products").mkdir(parents=True)
    child = OUTPUT + "/nested/child"
    if scenario == "nested":
        steps = [unpack(OUTPUT), unpack(child), replace_from_archive(child)]
    else:
        first = unpack(OUTPUT) if scenario == "duplicate" else marker(OUTPUT)
        steps = [marker("first.txt"), first, unpack(OUTPUT)]
    bundle = root / "kit"
    manifest = make_bundle(bundle, archive_bytes("tar"), steps)
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        args = blueprint_args(brw, bundle, manifest, work, mode)
        report = invoke(args, root, 0 if scenario == "nested" else 1)
        if scenario == "nested":
            assert report["status"] == ("planned" if preview else "applied"), report
            assert report["completed"] == (0 if preview else 3), report
            assert [s["path"] for s in report["steps"]] == [s["path"] for s in steps], report
            if not preview:
                assert (work / OUTPUT / "nested/input.txt").read_bytes() == MEMBERS[0][1]
                assert (work / child / "nested/input.txt").read_bytes() == b"read result\n"
                continue
        else:
            assert (report["status"], report["error"], report["completed"]) == ("failed", "destination-exists", 0), report
            assert report["detail"] == "main.mjs[1] > children/extract.mjs[3]", report
        assert snapshot(root, preview) == before, (scenario, mode)


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="brw-unpack-") as fixture:
        for scenario in ("tar", "gzip", "zip", "zip-empty", "zip-bad-crc", "zip-truncated",
                         "damaged", "outside", "digest", "existing", "empty"):
            for entrypoint in ("cli", "blueprint"):
                exercise(Path(fixture) / f"{scenario}-{entrypoint}", executable, scenario, entrypoint == "blueprint")
            print(f"{scenario}: CLI and Blueprint passed")
        for scenario in ("nested", "duplicate", "copy-conflict"):
            exercise_composition(Path(fixture) / scenario, executable, scenario)
            print(f"{scenario}: Blueprint composition passed")
