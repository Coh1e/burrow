#!/usr/bin/env python3
"""Platform-specific builder 2 payload for the shared Blueprint's local catalog."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile


def main():
    _, mode, _, output = sys.argv[1:]
    if mode not in ("Debug", "Release"):
        raise ValueError(f"unsupported build mode: {mode}")
    # Use the locked middle and base dependencies to invoke the original build payload.
    os.environ["BRW_ACCEPT_BUILDER"] = "2"
    subprocess.run(["middle", *sys.argv[1:]], check=True)
    archive_name = Path(output).read_text(encoding="utf-8").strip()
    if archive_name != f"{mode}.zip":
        raise ValueError(f"unexpected named archive: {archive_name}")

    with tempfile.TemporaryDirectory() as scratch:
        binary = Path(scratch) / "probe"
        source = '#include <stdio.h>\nint main(void) { return puts("brw Linux " MODE) == EOF; }\n'
        subprocess.run(["cc", "-std=c99", f'-DMODE="{mode}"', "-x", "c", "-", "-o", binary],
                       input=source, text=True, check=True)
        with zipfile.ZipFile(archive_name, "a") as archive:
            archive.write(binary, "bin/probe")


if __name__ == "__main__":
    main()
