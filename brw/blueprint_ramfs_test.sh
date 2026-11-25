#!/bin/sh
# ramfs supports distinct names differing in case and omits directory case flags.
set -eu
if ! command -v mount >/dev/null || ! unshare -Ur -m --propagation private true; then
    echo "Skipping ramfs test: private user/mount namespaces or mount tools unavailable"
    exit 77
fi

mountpoint=$(mktemp -d)
trap 'rmdir "$mountpoint"' EXIT
unshare -Ur -m --propagation private sh -ec '
    mount -t ramfs -o nodev,nosuid,noexec ramfs "$1" || {
        echo "Skipping ramfs test: private ramfs mount unavailable"
        exit 77
    }
    cd "$1"
    shift
    exec "$@"
' sh "$mountpoint" "$@"
