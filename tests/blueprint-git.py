"""Exercise managed Git worktrees through a verified Blueprint bundle."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tarfile
import tempfile

sys.dont_write_bytecode = True
from archive_fixture import blueprint_args, invoke, sha, snapshot


def git(directory, *args):
    return subprocess.check_output(
        ["git", "-C", directory, "-c", "user.name=Fixture", "-c",
         "user.email=fixture@example.invalid", *args], text=True
    ).strip()


def commit(directory, content):
    (directory / "tracked.txt").write_bytes(content)
    git(directory, "add", "tracked.txt")
    git(directory, "commit", "-qm", "fixture change")
    return git(directory, "rev-parse", "HEAD")


def bundle(root, *operations):
    kit = root / "kit"
    kit.mkdir(exist_ok=True)
    steps = [*operations, {"op": "pack", "format": "tar", "path": "result.tar",
                         "inputs": ["tree/tracked.txt"]}]
    module = ("export default () => " + json.dumps(steps)).encode("ascii")
    (kit / "main.mjs").write_bytes(module)
    capabilities = "".join(f"requires\t{op}\n" for op in sorted({step["op"] for step in steps}))
    manifest = ("brw-blueprint-v1\nentry\tmain.mjs\nplatform\tany\narch\tany\n"
                f"{capabilities}module\tmain.mjs\t{sha(module)}\n").encode("ascii")
    (kit / "blueprint.manifest").write_bytes(manifest)
    return kit, sha(manifest)


def exercise(brw, root):
    remote = root / "local repository"
    work = root / "workspace"
    for directory in (remote, work, root / "temp"):
        directory.mkdir()
    git(remote, "init", "-q")
    # The initial checkout can load attributes before this file exists.
    # With no line ending, autocrlf cannot change this file's own bytes.
    (remote / ".gitattributes").write_bytes(b"* -text")
    git(remote, "add", ".gitattributes")
    first = commit(remote, b"first\n")
    kit, digest = bundle(root, {"op": "git-clone", "path": "tree", "repository": str(remote)})

    def run(mode, code=0):
        return invoke(blueprint_args(brw, kit, digest, work, mode), root, code)

    def preview():
        before = snapshot(root, True)
        for mode in ("preview", "dry-run"):
            report = run(mode)
            assert report["completed"] == 0, report
            assert report["steps"][0]["remoteStatus"] == "unverified", report
            assert snapshot(root, True) == before, report

    def apply(expected_commit, expected_bytes):
        report = run("apply")
        assert report["completed"] == 2 and report["steps"][0]["commit"] == expected_commit, report
        result = work / "result.tar"
        with tarfile.open(result) as archive:
            assert archive.extractfile("tree/tracked.txt").read() == expected_bytes
        result.unlink()

    preview()
    apply(first, b"first\n")
    second = commit(remote, b"second\n")
    kit, digest = bundle(root, {"op": "git-pull", "path": "tree"})
    preview()
    apply(second, b"second\n")

    tree = work / "tree"
    tracked = tree / "tracked.txt"

    def rejects(reason, head, content):
        report = run("apply", 1)
        assert (report["error"], report["detail"], report["completed"]) == (reason, "main.mjs[1]", 0), report
        assert git(tree, "rev-parse", "HEAD") == head
        assert tracked.read_bytes() == content
        assert not (work / "result.tar").exists()

    tracked.write_bytes(b"dirty\n")
    rejects("dirty-worktree", second, b"dirty\n")
    local = commit(tree, b"local\n")
    commit(remote, b"remote\n")
    rejects("non-fast-forward", local, b"local\n")

    git(tree, "config", "core.worktree", str(remote))
    before = snapshot(root, True)
    report = run("apply", 1)
    assert (report["error"], report["completed"]) == ("unsafe-worktree", 0), report
    assert snapshot(root, True) == before, report
    git(tree, "config", "--unset", "core.worktree")

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.setblocking(False)
        url = f"http://127.0.0.1:{listener.getsockname()[1]}/repository"
        kit, digest = bundle(root, {"op": "git-clone", "path": "other", "repository": url})
        preview()
        git(tree, "remote", "set-url", "origin", url)
        kit, digest = bundle(root, {"op": "git-pull", "path": "tree"})
        preview()
        try:
            connection, _ = listener.accept()
        except BlockingIOError:
            pass
        else:
            connection.close()
            raise AssertionError("Git preview connected to the configured remote")

    combined = work / "nested"
    combined.mkdir()
    kit, digest = bundle(root,
                         {"op": "git-clone", "path": "nested/checkout", "repository": str(remote)},
                         {"op": "git-pull", "path": "nested/checkout"},
                         {"op": "replace", "path": "nested/checkout/tracked.txt",
                          "from": "remote", "to": "composed", "count": 1})
    preview()
    report = run("apply")
    assert report["completed"] == 4, report
    assert (combined / "checkout/tracked.txt").read_bytes() == b"composed\n"


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="brw-blueprint-git-") as directory:
        exercise(Path(sys.argv[1]).resolve(), Path(directory))
