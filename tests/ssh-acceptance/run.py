"""Verify a Windows OpenSSH transfer and offline build on a dedicated loopback WSL."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile
import threading
import uuid


ROOT = Path(__file__).resolve().parents[2]
PINS = "scripts/blueprint-sources.txt"
KIT = "tests/blueprint-acceptance/kit/blueprint.manifest"
SOURCE_PATHS = ("brw", PINS, "scripts/fetch-blueprint-deps.sh",
                "tests/blueprint-acceptance", "tests/ssh-acceptance")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def pack(path, revision, manifest):
    subprocess.run(["git", "archive", "--format=tar", f"--output={path}",
                    revision, "--", *SOURCE_PATHS], cwd=ROOT, check=True)
    with tarfile.open(path) as archive:
        require(hashlib.sha256(archive.extractfile(KIT).read()).hexdigest() == manifest,
                "the committed Blueprint does not match the trusted manifest digest")
        pins = archive.extractfile(PINS).read().decode("ascii").splitlines()
    with tarfile.open(path, "a") as archive:
        for line in pins:
            filename, expected, _url = line.split()
            member = Path(".local/deps/sources") / filename
            require(digest(ROOT / member) == expected, f"incorrect preseeded archive: {filename}")
            archive.add(ROOT / member, arcname=member.as_posix(), recursive=False)


def client(args, known_hosts):
    # Ignore ambient OpenSSH config, agents and global trust. This acceptance
    # entry point always addresses the operator's dedicated local WSL.
    hosts = known_hosts.resolve().as_posix()
    return ["ssh.exe", "-F", "none", "-T", "-p", str(args.port),
            "-i", str(args.identity.resolve()), "-o", "BatchMode=yes",
            "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none",
            "-o", "PreferredAuthentications=publickey", "-o", "ConnectionAttempts=1",
            "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=yes",
            "-o", "HostKeyAlgorithms=ssh-ed25519", "-o", "UpdateHostKeys=no",
            "-o", "GlobalKnownHostsFile=none", "-o", f'UserKnownHostsFile="{hosts}"',
            f"{args.user}@127.0.0.1"]


def execute(ssh, command, timeout, *, interrupt_on=None):
    # A lost exit status (including ssh's ambiguous exit 255) is not a
    # remote failure. Only the completion record supplies the remote status.
    tag = "brw-exit-" + uuid.uuid4().hex + "="
    wrapped = f"( {command} ); rc=$?; printf '\\n{tag}%d\\n' \"$rc\""
    lines = []
    interrupted = False
    with subprocess.Popen([*ssh, wrapped], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, encoding="utf-8", errors="replace") as proc:
        timer = threading.Timer(timeout, proc.kill)
        timer.start()
        try:
            for line in proc.stdout:
                line = line.rstrip("\r\n")
                if interrupt_on is not None and line == interrupt_on:
                    interrupted = True
                    proc.kill()
                lines.append(line)
            ssh_exit = proc.wait()
        except KeyboardInterrupt:
            proc.kill()
            proc.wait()
            raise RuntimeError("SSH command cancelled; remote result unknown, no retry") from None
        finally:
            timer.cancel()
            timer.join()
    last = lines[-1] if lines else ""
    completed = re.fullmatch(re.escape(tag) + r"(\d{1,3})", last)
    return {"result": "completed" if completed else "unknown",
            "remoteExit": int(completed[1]) if completed else None,
            "sshExit": ssh_exit, "interruptedAfterMarker": interrupted,
            "output": "\n".join(lines[:-1] if completed else lines).strip()}


def receive(ssh, remote, dest):
    # Extract only directories and ordinary files from the evidence archive.
    with tempfile.TemporaryFile() as stream:
        copy = subprocess.run([*ssh, f"tar -C {remote}/output/evidence -czf - ."],
                              stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.PIPE,
                              timeout=120)
        require(copy.returncode == 0, "could not retrieve the remote evidence")
        stream.seek(0)
        with tarfile.open(fileobj=stream, mode="r:gz") as archive:
            require(all(m.isdir() or m.isfile() for m in archive), "unexpected evidence file type")
            archive.extractall(dest, filter="data")


def run(args):
    require(os.name == "nt", "run the SSH client on Windows")
    require(re.fullmatch(r"[a-z_][a-z0-9_-]*", args.user) and 1 <= args.port <= 65535,
            "invalid WSL login name or port")
    require(re.fullmatch(r"[a-f0-9]{64}", args.manifest_sha256), "expected a SHA-256 digest")
    require(args.identity.is_file() and args.known_hosts.is_file(), "missing test identity or trust file")
    args.evidence.mkdir(parents=True, exist_ok=False)
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    remote = "/tmp/brw-132-" + uuid.uuid4().hex
    ssh = client(args, args.known_hosts)
    host = "127.0.0.1" if args.port == 22 else f"[127.0.0.1]:{args.port}"
    entries = args.known_hosts.read_text(encoding="ascii").strip().splitlines()
    require(len(entries) == 1 and entries[0].split()[:2] == [host, "ssh-ed25519"],
            "expected one trusted ed25519 entry for the dedicated loopback endpoint")
    key = subprocess.check_output(["ssh-keygen.exe", "-lf", str(args.known_hosts), "-E", "sha256"],
                                  text=True).split()[1]
    version = subprocess.check_output(["ssh.exe", "-V"], stderr=subprocess.STDOUT, text=True).strip()
    report = {"revision": revision, "client": version, "hostKeyFingerprint": key,
              "manifestSha256": args.manifest_sha256}
    report_file = args.evidence / "transport.json"
    write_json(report_file, report)

    with tempfile.TemporaryDirectory(dir=args.evidence) as scratch:
        empty = Path(scratch) / "empty_known_hosts"
        empty.touch()
        negative = subprocess.run([*client(args, empty), "true"], stdin=subprocess.DEVNULL,
                                  capture_output=True, timeout=30)
        require(negative.returncode == 255 and b"host key verification failed" in negative.stderr.lower(),
                "untrusted host identity did not fail verification")
        report["untrustedHostRejected"] = True
        probe = execute(ssh, "uname -srm", 30)
        require(probe["remoteExit"] == 0 and "microsoft" in probe["output"].lower(),
                "trusted SSH endpoint did not identify itself as WSL")
        report["host"] = probe["output"]
        source = Path(scratch) / "materials.tar"
        pack(source, revision, args.manifest_sha256)
        checksum = digest(source)
        with source.open("rb") as data:
            upload = subprocess.run([*ssh, f"umask 077 && mkdir {remote} && cd {remote} && "
                                     "cat > materials.tar && sha256sum materials.tar"],
                                    stdin=data, capture_output=True, timeout=180)
        require(upload.returncode == 0 and upload.stdout.split() == [checksum.encode(), b"materials.tar"],
                "remote materials digest did not match the Windows archive")
        report["transfer"] = {"bytes": source.stat().st_size, "windowsSha256": checksum,
                              "wslSha256": upload.stdout.split()[0].decode("ascii")}
        write_json(report_file, report)

    isolated = (f"set -eu; printf '%s  materials.tar\\n' {checksum} | sha256sum -c -; "
                "mkdir source; tar -xf materials.tar -C source; "
                f"exec sh source/tests/ssh-acceptance/build.sh {remote}/output {args.manifest_sha256}")
    command = (f"cd {remote} && export BRW_SSH_TRANSPORT_NETNS=$(readlink /proc/self/ns/net) && "
               f"exec unshare -Urn sh -c {shlex.quote(isolated)}")
    built = execute(ssh, command, 1800)
    # OpenSSH diagnostics on a failed session can contain the local identity
    # path or login. Store only the outcome and the fixed remote build logs.
    if built["result"] == "unknown":
        built.pop("output")
    report["build"] = built
    write_json(report_file, report)
    if built["result"] == "unknown":
        raise RuntimeError("remote build result unknown; no retry")
    if built["remoteExit"] != 0:
        try:
            receive(ssh, remote, args.evidence / "remote")
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            raise RuntimeError(f"remote build exited {built['remoteExit']}; evidence unavailable") from error
        raise RuntimeError(f"remote build failed with exit {built['remoteExit']}")
    evidence = args.evidence / "remote"
    receive(ssh, remote, evidence)
    product = json.loads((evidence / "linux/linux-product.json").read_text(encoding="utf-8"))
    require(product["brwSha256"] == (evidence / "brw.sha256").read_text().split()[0] and
            product["manifestSha256"] == args.manifest_sha256 and product["exit"] == 0,
            "remote product record does not match the fresh build and trusted Blueprint")

    marker = "started-" + uuid.uuid4().hex
    # This fixed harmless operation acknowledges a durable start, then leaves
    # the SSH channel open so the client can drop it before the exit record.
    interrupted = execute(ssh, f"cd {remote} && echo started >> attempts && echo {marker} && sleep 5",
                          30, interrupt_on=marker)
    interrupted.pop("output")
    report["disconnect"] = interrupted
    write_json(report_file, report)
    require(interrupted["interruptedAfterMarker"] and interrupted["result"] == "unknown",
            "disconnect did not yield an unknown remote result")
    observed = execute(ssh, f"wc -l < {remote}/attempts", 30)
    require(observed["remoteExit"] == 0 and observed["output"] == "1",
            "expected exactly one remote start after disconnect")
    report["disconnect"]["observedStarts"] = 1
    control = execute(ssh, "exit 255", 30)
    control.pop("output")
    report["knownFailure"] = control
    write_json(report_file, report)
    require(control["remoteExit"] == 255, "confirmed remote exit 255 was lost")
    print("Offline build and Linux product passed. Disconnected command: result unknown, one start.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--user", required=True)
    parser.add_argument("--identity", required=True, type=Path)
    parser.add_argument("--known-hosts", required=True, type=Path)
    parser.add_argument("--manifest-sha256", required=True)
    parser.add_argument("--evidence", required=True, type=Path)
    try:
        run(parser.parse_args())
    except subprocess.TimeoutExpired:
        sys.exit("SSH transport timed out; result unknown, no retry")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
