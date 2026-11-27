"""Apply the shared Blueprint and run the Linux program from its named Release output."""

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import zipfile


SHARED = Path(__file__).resolve().parents[1] / "blueprint-acceptance"
sys.dont_write_bytecode = True
sys.path.insert(0, str(SHARED))
import fixture
from run import Case, digest
import scenarios


class PinnedCase(Case):
    def __init__(self, root, evidence, brw, env, trusted_manifest):
        super().__init__(root, evidence, brw, env)
        self.trusted_manifest = trusted_manifest

    def blueprint(self, label, mode, expected=0):
        result = self.run(label, [self.brw, "blueprint", mode, self.kit, "--sha256",
                                  self.trusted_manifest, "--workspace", self.work, "--json"], expected)
        return json.loads(result.stdout)


def lock_linux_builder(case):
    catalog = case.root / "catalog" / "catalog"
    builder = catalog.parent / "builder-2.bin"
    original_sha = digest(builder)
    shutil.copyfile(Path(__file__).with_name("linux_builder.py"), builder)
    rows = catalog.read_text(encoding="ascii")
    if rows.count(original_sha) != 1:
        raise ValueError("expected one builder 2 artifact in the local catalog")
    catalog.write_text(rows.replace(original_sha, digest(builder)), encoding="ascii")
    case.blueprint("lock-linux-builder", "lock")


def run(brw, root, evidence, trusted_manifest):
    if sys.platform != "linux" or not brw.is_absolute() or not brw.is_file():
        raise ValueError("--brw must be an absolute path to a Linux brw executable")
    if digest(SHARED / "kit" / "blueprint.manifest") != trusted_manifest:
        raise ValueError("shared Blueprint manifest SHA-256 mismatch")
    if root == evidence or root.exists() or evidence.exists():
        raise ValueError("--work and --evidence must be distinct, new directories")

    case = PinnedCase(root, evidence, brw, os.environ, trusted_manifest)
    # The cross-platform kit calls 'python'. Some Linux distributions provide only python3.
    python_bin = root / "python-bin"
    python_bin.mkdir()
    (python_bin / "python").symlink_to(sys.executable)
    case.env["PATH"] = str(python_bin) + os.pathsep + case.env["PATH"]
    fixture.prepare(case)
    lock_linux_builder(case)
    scenarios.main(case)

    calls = [json.loads(line) for line in (case.work / "calls.jsonl").read_text(encoding="utf-8").splitlines()]
    consumer = [event for event in calls if event["action"] == "test"][-1]
    if (consumer["mode"], consumer["archive"]) != ("Release", "Release.zip"):
        raise ValueError("consumer did not receive the Release archive")
    archive = case.work / consumer["archive"]
    product = root / "release-probe"
    with zipfile.ZipFile(archive) as package:
        binary = package.read("bin/probe")
    if not binary.startswith(b"\x7fELF"):
        raise ValueError("Release product is not an ELF executable")
    product.write_bytes(binary)
    product.chmod(0o700)
    result = case.run("run-linux-product", [product])
    if result.stdout != "brw Linux Release\n":
        raise ValueError(f"unexpected output from the Release product: {result.stdout!r}")
    case.record("linux-product", {
        "platform": scenarios.TARGET, "manifestSha256": trusted_manifest, "brwSha256": digest(brw),
        "lockSha256": digest(case.kit / f"blueprint.{scenarios.TARGET}.lock"),
        "producer": "main.mjs[7]", "output": "archive",
        "archive": archive.name, "archiveSha256": digest(archive),
        "member": "bin/probe", "format": "ELF", "elfSha256": digest(product),
        "stdout": result.stdout, "exit": result.returncode,
    })


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--brw", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--manifest-sha256", required=True)
    args = parser.parse_args()
    run(args.brw, args.work.resolve(), args.evidence.resolve(), args.manifest_sha256)
