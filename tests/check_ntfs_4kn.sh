#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The native Common V2 formatter must create a genuine 4Kn NTFS volume.
set -eu
test_app="${1:?native Common V2 formatter test executable required}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"$test_app" "$work/ntfs-4kn.img" --sector-4096
