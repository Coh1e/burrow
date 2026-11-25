#!/bin/sh
set -eu
if [ "$(stat -f -c %t .)" != 1021997 ] || ! command -v python3 >/dev/null; then
    echo "Skipping unknown case test: requires a Linux 9p build directory and python3"
    exit 77
fi

# Check metadata availability independently of the CLI under test.
python3 - <<'PY'
import array
import errno
import fcntl
import os
import sys
import tempfile

with tempfile.TemporaryDirectory(dir=".") as directory:
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        flags = array.array("l", [0])
        getflags = 0x80006601 | (flags.itemsize << 16)  # _IOR('f', 1, long) on WSL
        for query in (lambda: os.getxattr(fd, "system.wsl_case_sensitive"),
                      lambda: fcntl.ioctl(fd, getflags, flags)):
            try:
                query()
            except OSError as exc:
                if exc.errno not in (errno.ENODATA, errno.ENOTSUP, errno.ENOTTY):
                    raise
            else:
                print("Skipping unknown case test: directory supplies case metadata")
                sys.exit(77)
    finally:
        os.close(fd)
PY
exec "$@"
