#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The native Common V2 formatter must create a genuine 4Kn NTFS volume.
set -eu
test_app="${1:?native Common V2 formatter test executable required}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
sector="${2:-4096}"
case "$sector" in
    1024|2048|4096) ;;
    *) echo "Unsupported NTFS logical-sector regression: $sector" >&2; exit 2 ;;
esac
"$test_app" "$work/ntfs-${sector}.img" "--sector-${sector}"
