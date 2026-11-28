"""Verify independent downloads through the full CLI and a nested Blueprint."""

from contextlib import contextmanager
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tarfile
import tempfile
import threading

sys.dont_write_bytecode = True
from archive_fixture import blueprint_args, fixture_env, invoke, sha, snapshot


DATA = bytes(range(256)) * 1024
PREFIX = DATA[:32768]
OUTPUT = "files/received bytes"
RECEIPT = "files/receipt.tar"
SOURCE = "main.mjs[1] > fetch.mjs[1]"
RIVAL = b"written by another process"


class Response(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_HEAD(self):
        # A request queued after the client exits also counts earlier bare TCP
        # connections. Exclude the fixture's own checkpoint from the count.
        assert self.path == "/checkpoint"
        self.server.accepted -= 1
        self.send_response(200)
        self.send_header("Connections", str(self.server.accepted))
        self.end_headers()

    def do_GET(self):
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/artifact")
            self.end_headers()
            return
        self.send_response(200)
        limit = 64 * 1024 * 1024
        self.send_header("Content-Length", str(limit + 1 if self.path == "/large" else len(DATA)))
        self.end_headers()
        if self.path == "/truncated":
            self.wfile.write(PREFIX)
        elif self.path == "/large":
            try:
                for _ in range(limit // len(DATA)):
                    self.wfile.write(DATA)
                self.wfile.write(b"!")
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # A transport can reject a known excessive length before reading the body.
        elif self.path == "/held":
            try:
                self.wfile.write(PREFIX)
                self.server.started.set()
                # Cancellation must finish while this response is silent.
                if self.server.release.wait(25):
                    self.wfile.write(DATA[len(PREFIX):])
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
        else:
            assert self.path == "/artifact"
            self.wfile.write(DATA)


class LocalHTTP(HTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), Response)
        self.url = f"http://127.0.0.1:{self.server_port}"
        self.accepted = 0
        self.started = threading.Event()
        self.release = threading.Event()

    def get_request(self):
        sock, address = super().get_request()
        sock.settimeout(5)
        self.accepted += 1
        return sock, address

    def connections(self):
        client = HTTPConnection(*self.server_address, timeout=10)
        try:
            client.request("HEAD", "/checkpoint")
            return int(client.getresponse().getheader("Connections"))
        finally:
            client.close()


@contextmanager
def serving():
    with LocalHTTP() as server:
        thread = threading.Thread(target=server.serve_forever, args=(0.05,))
        thread.start()
        try:
            yield server
        finally:
            server.release.set()
            server.shutdown()
            thread.join()


def bundle_at(bundle, url, digest, destination, preceding_copy=False):
    bundle.mkdir()
    steps = [{"op": "download", "url": url, "sha256": digest, "path": destination},
             {"op": "pack", "format": "tar", "inputs": [OUTPUT], "path": RECEIPT}]
    if preceding_copy:
        steps.insert(0, {"op": "copy", "payload": "./marker", "path": "copy-ran"})
    modules = {"main.mjs": b'export default () => [{op:"include",module:"fetch.mjs",args:{}}];',
               "fetch.mjs": ("export default () => " + json.dumps(steps)).encode("ascii")}
    rows = ["brw-blueprint-v1", "entry\tmain.mjs", "platform\tany", "arch\tany",
            "requires\tdownload", "requires\tpack"]
    if preceding_copy:
        marker = b"copy completed"
        (bundle / "marker").write_bytes(marker)
        rows += ["requires\tcopy", f"payload\tmarker\t{sha(marker)}"]
    for name, data in modules.items():
        (bundle / name).write_bytes(data)
        rows.append(f"module\t{name}\t{sha(data)}")
    manifest = ("\n".join(rows) + "\n").encode("ascii")
    (bundle / "blueprint.manifest").write_bytes(manifest)
    return sha(manifest)


def interrupt_or_occupy(args, root, server, cancel):
    marker = root.parent / "send-break"
    server.started.clear()
    server.release.clear()
    if cancel and os.name == "nt":
        # The native helper creates a dedicated process group even under CTest
        # with redirected pipes and sends Ctrl+Break when the marker appears.
        args = [args[0].with_name("brw_test_cancel.exe"), args[0], marker, *args[1:]]
    with subprocess.Popen(args, cwd=root, env=fixture_env(root), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as child:
        try:
            assert server.started.wait(10), "download never reached the partial HTTP response"
            target = root / "workspace" / OUTPUT
            assert not target.exists(), "partial download was published"
            if not cancel:
                with target.open("xb") as winner:
                    winner.write(RIVAL)
                server.release.set()
            elif os.name == "nt":
                marker.touch()
            else:
                child.send_signal(signal.SIGINT)
            out, err = child.communicate(timeout=20)
            assert child.returncode == 1, (child.returncode, out, err)
            return json.loads(out)
        finally:
            server.release.set()
            if child.poll() is None:
                child.kill()
            if marker.exists():
                marker.unlink()


def exercise(brw, root, server, blueprint, scenario):
    work = root / "workspace"
    (work / "files").mkdir(parents=True)
    (work / "files/sibling").write_bytes(b"previous contents")
    (root / "temp").mkdir()
    if scenario == "occupied":
        (work / OUTPUT).write_bytes(b"original file")
    route = {"partial": "truncated", "cancel": "held", "late-occupant": "held",
             "too-large": "large", "redirect": "redirect"}.get(scenario, "artifact")
    url = server.url + "/" + route
    if scenario == "invalid-url":
        url = server.url + "/artifact#fragment"
    elif scenario == "malformed-ipv6":
        url = "http://[invalid]/file"
    elif scenario == "offline-ipv6":
        url = f"http://[::1]:{server.server_port}/artifact"
    destination = "files/../outside" if scenario == "unsafe-path" else OUTPUT
    # The truncated bytes have the requested digest: an incomplete HTTP
    # response must still fail before publication.
    digest = sha({"mismatch": b"wrong bytes", "partial": PREFIX}.get(scenario, DATA))
    if scenario == "invalid-digest":
        digest = "Z" * 64
    local_error = {"occupied": "destination-exists", "invalid-url": "download-url", "malformed-ipv6": "download-url",
                   "unsafe-path": "unsafe-destination", "invalid-digest": "invalid-digest"}.get(scenario)
    remote_error = {"mismatch": "file-digest-mismatch", "partial": "download-failed",
                    "redirect": "download-failed", "cancel": "cancelled", "too-large": "download-limit",
                    "late-occupant": "destination-exists"}.get(scenario)
    bundle = root / "kit"
    manifest_digest = bundle_at(bundle, url, digest, destination) if blueprint else None
    modes = ("preview", "dry-run", "apply") if blueprint else ("dry-run", "apply")
    if scenario == "offline-ipv6":
        modes = modes[:-1]  # The fixture serves IPv4 only. Preview needs no reachable IPv6 endpoint.
    for mode in modes:
        preview = mode != "apply"
        error = local_error or (remote_error if not preview else None)
        before = snapshot(root, preview)
        connections = server.connections()
        if blueprint:
            args = blueprint_args(brw, bundle, manifest_digest, work, mode)
        else:
            args = [brw, "download", url, destination, "--sha256", digest, "--workspace", work, "--json"]
            if preview:
                args.append("--dry-run")
        if not preview and scenario in ("cancel", "late-occupant"):
            report = interrupt_or_occupy(args, root, server, cancel=scenario == "cancel")
        else:
            report = invoke(args, root, 1 if error else 0)
        assert server.connections() - connections == int(not preview and not local_error), (mode, scenario)
        if error:
            assert (report["status"], report["error"]) == ("failed", error), report
            if blueprint:
                assert (report["detail"], report["completed"]) == (SOURCE, 0), report
        else:
            step = report["steps"][0] if blueprint else report
            assert report["status"] == ("planned" if preview else "applied"), report
            assert (step["url"], step["sha256"], step["path"]) == (url, digest, OUTPUT), report
            if preview:
                assert step["remoteStatus"] == "unverified", report
            if blueprint:
                assert (step["op"], step["source"]) == ("download", SOURCE), report
                assert report["completed"] == (0 if preview else 2), report
                if preview:
                    assert report["steps"][1]["status"] == "check-at-execution", report
        if preview or error:
            after = snapshot(root, preview)
            if not preview and scenario == "late-occupant":
                assert after.pop(Path("workspace") / OUTPUT)[1] == RIVAL
            assert after == before, (scenario, mode)
        else:
            assert (work / OUTPUT).read_bytes() == DATA
            after = snapshot(root, False)
            del after[Path("workspace") / OUTPUT]
            if blueprint:
                with tarfile.open(work / RECEIPT, "r:") as archive:
                    assert archive.getnames() == [OUTPUT]
                    assert archive.extractfile(OUTPUT).read() == DATA
                del after[Path("workspace") / RECEIPT]
            assert after == before, scenario


def exercise_missing_curl(brw, root, server):
    work = root / "workspace"
    (work / "files").mkdir(parents=True)
    (root / "temp").mkdir()
    empty_path = root / "empty-bin"
    empty_path.mkdir()
    env = fixture_env(root)
    env["PATH"] = str(empty_path)
    url = server.url + "/artifact"
    digest = sha(DATA)
    bundle = root / "kit"
    manifest = bundle_at(bundle, url, digest, OUTPUT, preceding_copy=True)
    cli = [brw, "download", url, OUTPUT, "--sha256", digest, "--workspace", work, "--json"]
    commands = [cli + ["--dry-run"], cli]
    commands += [blueprint_args(brw, bundle, manifest, work, mode) for mode in ("preview", "dry-run", "apply")]
    before = snapshot(root, True)
    connections = server.connections()
    for args in commands:
        run = subprocess.run(args, cwd=root, env=env, capture_output=True, timeout=25)
        assert run.returncode == 1, (args, run.returncode, run.stdout, run.stderr)
        report = json.loads(run.stdout)
        assert (report["status"], report["error"]) == ("failed", "missing-capability"), report
        if args[1] == "blueprint":
            assert (report["completed"], report["detail"]) == (0, "download"), report
        assert snapshot(root, True) == before, "missing transport permitted workspace or staging writes"
        assert server.connections() == connections, "missing transport attempted a connection"


if __name__ == "__main__":
    brw = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="brw-download-") as directory, serving() as server:
        for scenario in ("success", "mismatch", "partial", "occupied", "too-large", "cancel",
                         "late-occupant", "redirect", "invalid-url", "invalid-digest", "unsafe-path",
                         "malformed-ipv6", "offline-ipv6"):
            for blueprint in (False, True):
                exercise(brw, Path(directory) / f"{scenario}-{blueprint}", server, blueprint, scenario)
            print(f"{scenario}: CLI and Blueprint passed")
        if os.name == "posix":
            exercise_missing_curl(brw, Path(directory) / "missing-curl", server)
            print("missing curl: CLI and Blueprint refused before workspace writes")
