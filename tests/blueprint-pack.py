"""Exercise archive creation through the full CLI and a nested Blueprint."""

import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import zipfile

sys.dont_write_bytecode = True
from archive_fixture import blueprint_args, invoke, sha, snapshot


FILES = {"inputs/first file": b"\x00first\xff\n" * 4096, "inputs/last.txt": b"last\n", "empty": b""}
OUTPUT = "products/new archive"
RECEIPT = "products/receipt.tar"
CALL = "main.mjs[1] > child.mjs"


def contents(data, fmt):
    if fmt == "zip":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            return [(name, archive.read(name)) for name in archive.namelist()]
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:" if fmt == "tar" else "r:gz") as archive:
        return [(entry.name, archive.extractfile(entry).read()) for entry in archive]


def pack(fmt, path, inputs):
    return {"op": "pack", "format": fmt, "path": path, "inputs": list(inputs)}


def bundle_at(bundle, steps, seed=b"from previous step"):
    bundle.mkdir()
    files = {"main.mjs": b'export default () => [{op:"include",module:"child.mjs",args:{}}];',
             "child.mjs": ("export default () => " + json.dumps(steps)).encode("ascii"),
             "seed.txt": seed}
    rows = ["brw-blueprint-v1", "entry\tmain.mjs", "platform\tany", "arch\tany", "requires\tpack"]
    for op in sorted({s["op"] for s in steps} - {"pack"}):
        rows.append("requires\t" + op)
    for name, data in files.items():
        (bundle / name).write_bytes(data)
        kind = "module" if name.endswith(".mjs") else "payload"
        rows.append(f"{kind}\t{name}\t{sha(data)}")
    manifest = ("\n".join(rows) + "\n").encode("ascii")
    (bundle / "blueprint.manifest").write_bytes(manifest)
    return sha(manifest)


def setup(root):
    root.mkdir()
    (root / "temp").mkdir()
    work = root / "workspace"
    (work / "inputs").mkdir(parents=True)
    (work / "products").mkdir()
    (work / "products/keep.txt").write_bytes(b"sibling")
    for name, data in FILES.items():
        (work / name).write_bytes(data)
    return work


def exercise(brw, root, fmt, scenario, blueprint):
    work = setup(root)
    sources = list(FILES)
    if scenario in ("missing-last", "existing-missing-last"):
        sources[-1] = "no-such-file"
    if scenario in ("existing", "existing-missing-last"):
        (work / OUTPUT).write_bytes(b"previous archive")
    elif scenario == "duplicate":
        sources.append(sources[0])
    elif scenario == "many":
        for i in range(15):
            name = f"inputs/extra-{i}"
            (work / name).write_bytes(bytes([i]))
            sources.append(name)
    errors = {"missing-last": "archive-input-unavailable", "existing": "destination-exists",
              "existing-missing-last": "destination-exists", "duplicate": "archive-path-conflict"}
    error = errors.get(scenario)
    steps = [pack(fmt, OUTPUT, sources), pack("tar", RECEIPT, [OUTPUT])]
    bundle = root / "kit"
    if blueprint:
        digest = bundle_at(bundle, steps)
    modes = ("preview", "dry-run", "apply") if blueprint else ("dry-run", "apply")
    for mode in modes:
        preview = mode != "apply"
        before = snapshot(root, preview)
        if blueprint:
            args = blueprint_args(brw, bundle, digest, work, mode)
        else:
            args = [brw, "pack", OUTPUT, *sources, "--format", fmt, "--workspace", work, "--json"]
            if preview:
                args.append("--dry-run")
        report = invoke(args, root, 1 if error else 0)
        if error:
            assert (report["status"], report["error"]) == ("failed", error), report
            if blueprint:
                assert (report["detail"], report["completed"]) == (CALL + "[1]", 0), report
        else:
            assert report["status"] == ("planned" if preview else "applied"), report
            step = report["steps"][0] if blueprint else report
            assert (step["format"], step["path"], step["inputs"]) == (fmt, OUTPUT, sources), step
            if blueprint:
                assert step["op"] == "pack" and step["source"] == CALL + "[1]", report
                assert report["completed"] == (0 if preview else 2), report
            else:
                assert step["operation"] == "pack", report
        if error or preview:
            assert snapshot(root, preview) == before, (scenario, fmt, mode)
        else:
            archive = (work / OUTPUT).read_bytes()
            assert contents(archive, fmt) == [(name, (work / name).read_bytes()) for name in sources]
            if blueprint:
                assert contents((work / RECEIPT).read_bytes(), "tar") == [(OUTPUT, archive)]
            expected = {"keep.txt", Path(OUTPUT).name} | ({Path(RECEIPT).name} if blueprint else set())
            assert {p.name for p in (work / "products").iterdir()} == expected
            assert (work / "products/keep.txt").read_bytes() == b"sibling"
            if scenario == "ok" and not blueprint:
                unpack = [brw, "unpack", work / OUTPUT, "expanded", "--sha256", sha(archive),
                          "--workspace", work, "--json"]
                assert invoke(unpack, root, 0)["status"] == "applied"
                for name in sources:
                    assert (work / "expanded" / name).read_bytes() == FILES[name]


def exercise_producer(brw, root, bad_last):
    work = setup(root)
    sources = ["generated", "missing" if bad_last else "empty"]
    steps = [{"op": "copy", "path": "generated", "payload": "./seed.txt"},
             pack("tar", OUTPUT, sources)]
    bundle = root / "kit"
    digest = bundle_at(bundle, steps)
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        report = invoke(blueprint_args(brw, bundle, digest, work, mode), root, int(bad_last))
        if bad_last:
            assert (report["error"], report["completed"], report["detail"]) == (
                "archive-input-unavailable", 0, CALL + "[2]"), report
        else:
            assert report["status"] == ("planned" if preview else "applied"), report
        if preview or bad_last:
            assert snapshot(root, preview) == before
        else:
            assert contents((work / OUTPUT).read_bytes(), "tar") == [
                ("generated", b"from previous step"), ("empty", b"")]


def exercise_limits_and_links(brw, root):
    work = setup(root)
    # The input alone fits the payload limit; TAR headers push the output over it.
    with (work / "large").open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    os.link(work / "empty", work / "linked")
    # No files need creating to test the operand boundary. At 4,096 members
    # including the parent, validation reaches the first missing input.
    paths = [f"d/{i:x}" for i in range(4096)]
    cases = [("encoded-limit", ["large"], "archive-limit"),
             ("hardlink", ["inputs/first file", "linked"], "archive-input-unavailable"),
             ("at-member-limit", paths[:-1], "archive-input-unavailable"),
             ("over-member-limit", paths, "archive-limit")]
    if os.name == "posix":
        (work / "symbolic").symlink_to("inputs/first file")
        cases.append(("symlink", ["inputs/first file", "symbolic"], "archive-input-unavailable"))

    # Stage and final output can appear in products/ or the temporary directory.
    # Include both directories' own timestamps to catch transient preview writes.
    def output_snapshot(preview):
        return [(path.stat().st_mtime_ns if preview else None, snapshot(path, preview))
                for path in (work / "products", root / "temp")]

    for label, inputs, error in cases:
        bundle = root / label
        digest = bundle_at(bundle, [pack("tar", OUTPUT, inputs)])
        cli = [brw, "pack", OUTPUT, *inputs, "--format", "tar", "--workspace", work, "--json"]
        for args, preview in [(cli + ["--dry-run"], True), (cli, False),
                              (blueprint_args(brw, bundle, digest, work, "preview"), True),
                              (blueprint_args(brw, bundle, digest, work, "apply"), False)]:
            before = output_snapshot(preview)
            report = invoke(args, root, 1)
            assert (report["status"], report["error"]) == ("failed", error), (label, report)
            if "steps" in report:
                assert (report["completed"], report["detail"]) == (0, CALL + "[1]"), report
            assert output_snapshot(preview) == before, label
    assert (work / "large").stat().st_size == 64 * 1024 * 1024
    assert (work / "linked").samefile(work / "empty")


def exercise_input_changed(brw, root):
    work = setup(root)
    last = "inputs/last.txt"
    move = "from pathlib import Path; Path('inputs/last.txt').rename('moved.txt')"
    steps = [{"op": "run", "program": sys.executable, "args": ["-c", move]},
             pack("tar", OUTPUT, ["inputs/first file", last]), pack("tar", RECEIPT, [OUTPUT])]
    bundle = root / "kit"
    digest = bundle_at(bundle, steps)
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        report = invoke(blueprint_args(brw, bundle, digest, work, mode), root, 0 if preview else 1)
        if preview:
            assert report["status"] == "planned" and report["steps"][1]["status"] == "check-at-execution", report
        else:
            assert (report["error"], report["completed"], report["detail"]) == (
                "archive-input-unavailable", 1, CALL + "[2]"), report
            before[Path("workspace/moved.txt")] = before.pop(Path("workspace") / last)
        assert snapshot(root, preview) == before


def exercise_reused_destination(brw, root, move_output, consumer):
    work = setup(root)
    script = "from pathlib import Path; Path('bundle.tar').rename('previous.tar')" if move_output else "pass"
    seed = b"replacement"
    if consumer == "unpack":
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("member.txt", seed)
        seed = buffer.getvalue()
    last = (pack("tar", "bundle.tar", ["inputs/last.txt"]) if consumer == "pack" else
            {"op": consumer, "path": "bundle.tar", "payload": "./seed.txt"})
    steps = [pack("tar", "bundle.tar", ["empty"]),
             {"op": "run", "program": sys.executable, "args": ["-c", script]},
             last]
    bundle = root / "kit"
    digest = bundle_at(bundle, steps, seed)
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        report = invoke(blueprint_args(brw, bundle, digest, work, mode), root, int(not preview and not move_output))
        if preview:
            if consumer == "pack":
                assert report["steps"][2]["status"] == "check-at-execution", report
            assert snapshot(root, True) == before
        elif move_output:
            assert report["completed"] == 3, report
            assert contents((work / "previous.tar").read_bytes(), "tar") == [("empty", b"")]
            output = work / "bundle.tar"
            if consumer == "pack":
                assert contents(output.read_bytes(), "tar") == [("inputs/last.txt", FILES["inputs/last.txt"])]
            elif consumer == "unpack":
                assert (output / "member.txt").read_bytes() == b"replacement"
            else:
                assert output.read_bytes() == seed
        else:
            assert (report["error"], report["detail"], report["completed"]) == (
                "destination-exists", CALL + "[3]", 2), report
            assert contents((work / "bundle.tar").read_bytes(), "tar") == [("empty", b"")]
            assert set(snapshot(root, False)) == set(before) | {Path("workspace/bundle.tar")}


def exercise_new_parent_case(brw, root):
    work = setup(root)
    case_insensitive = (work / "EMPTY").exists()
    create = "from pathlib import Path; Path('created').mkdir()"
    steps = [{"op": "run", "program": sys.executable, "args": ["-c", create]},
             pack("tar", "created/One.tar", ["empty"]),
             pack("tar", RECEIPT, ["created/one.tar"])]
    bundle = root / "kit"
    digest = bundle_at(bundle, steps)
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        expected = int(not preview and not case_insensitive)
        report = invoke(blueprint_args(brw, bundle, digest, work, mode), root, expected)
        if preview:
            assert report["steps"][2]["status"] == "check-at-execution", report
            assert snapshot(root, True) == before
        else:
            archive = (work / "created/One.tar").read_bytes()
            assert contents(archive, "tar") == [("empty", b"")]
            if case_insensitive:
                assert report["completed"] == 3, report
                assert contents((work / RECEIPT).read_bytes(), "tar") == [("created/one.tar", archive)]
            else:
                assert (report["error"], report["detail"], report["completed"]) == (
                    "archive-input-unavailable", CALL + "[3]", 2), report
                created = Path("workspace/created")
                assert set(snapshot(root, False)) == set(before) | {created, created / "One.tar"}


def exercise_command_order(brw, root, command_after_pack):
    work = setup(root)
    archive = pack("tar", "bundle", ["empty"])
    move_into_directory = ("from pathlib import Path; p = Path('bundle'); "
                           "staged = p.rename('built.tar'); p.mkdir(); staged.rename(p / 'built.tar')")
    command = {"op": "run", "program": sys.executable,
               "args": ["-c", move_into_directory] if command_after_pack else ["--version"]}
    prefix = [archive, command] if command_after_pack else [command, archive]
    member = "bundle/built.tar"
    bundle = root / "kit"
    digest = bundle_at(bundle, [*prefix, pack("tar", RECEIPT, [member])])
    for mode in ("preview", "dry-run", "apply"):
        preview = mode != "apply"
        before = snapshot(root, preview)
        report = invoke(blueprint_args(brw, bundle, digest, work, mode), root, 0 if command_after_pack else 1)
        if not command_after_pack:
            assert (report["error"], report["detail"], report["completed"]) == (
                "archive-input-unavailable", CALL + "[3]", 0), report
        elif preview:
            assert report["steps"][2]["status"] == "check-at-execution", report
        else:
            archived = (work / member).read_bytes()
            assert report["completed"] == 3 and contents(archived, "tar") == [("empty", b"")], report
            assert contents((work / RECEIPT).read_bytes(), "tar") == [(member, archived)]
        if preview or not command_after_pack:
            assert snapshot(root, preview) == before


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="brw-pack-") as directory:
        root = Path(directory)
        for fmt, scenario in [("tar", "ok"), ("tar.gz", "ok"), ("zip", "ok"), ("tar", "many"),
                              ("tar", "missing-last"), ("tar", "existing"), ("tar", "existing-missing-last"),
                              ("tar", "duplicate")]:
            for blueprint in (False, True):
                exercise(executable, root / f"{fmt}-{scenario}-{blueprint}", fmt, scenario, blueprint)
            print(f"{fmt} {scenario}: CLI and Blueprint passed")
        for bad_last in (False, True):
            exercise_producer(executable, root / f"copy-pack-{bad_last}", bad_last)
        print("copy followed by pack: published input and missing final input passed")
        exercise_input_changed(executable, root / "input-changed")
        print("input moved by an earlier step: rollback and stop passed")
        exercise_limits_and_links(executable, root / "boundaries")
        print("archive byte limit, member count and linked inputs passed")
        for after_pack in (True, False):
            exercise_command_order(executable, root / f"command-after-pack-{after_pack}", after_pack)
        print("pack input type before and after an external command: passed")
        exercise_new_parent_case(executable, root / "new-parent-case")
        print("consuming an archive by case under a directory created at execution: passed")
        for consumer in ("pack", "copy", "unpack"):
            for moved in (True, False):
                exercise_reused_destination(executable, root / f"reused-{consumer}-{moved}", moved, consumer)
        print("pack, copy and unpack after an external command moves or retains an archive: passed")
