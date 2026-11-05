#!/usr/bin/env python3
"""Exercise the provider's nested-to-flat upgrade in an isolated dpkg root."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import build as fsbuild

if os.geteuid() != 0:
    raise SystemExit("Run this isolated dpkg integration test as root")
package = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="filesystem-flat-upgrade-") as temporary:
    work = Path(temporary)
    image, system = work / "old", work / "system"
    (image / "DEBIAN").mkdir(parents=True)
    (image / "DEBIAN/control").write_text(
        f"Package: {fsbuild.PACKAGE}\nVersion: 0.0.0\nArchitecture: amd64\n"
        "Maintainer: Fixture <fixture@example.invalid>\nDescription: isolated old documentation layout\n")
    resource = Path("System/Filesystems") / fsbuild.NAME
    old = Path("System/Resources/Documentation") / fsbuild.REPO / fsbuild.PDF.name
    (image / resource).parent.mkdir(parents=True)
    (image / resource).write_bytes(b"old filesystem module")
    (image / old).parent.mkdir(parents=True)
    (image / old).write_bytes(b"%PDF-1.4 old manual")
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", image, work / "old.deb"], check=True)
    subprocess.run(["dpkg", "--root", system, "--unpack", work / "old.deb"], check=True)
    personal = system / "System/Resources/Documentation/Personal.pdf"
    personal.write_bytes(b"personal document")
    subprocess.run(["dpkg", "--root", system, "--unpack", package], check=True)
    flat = Path("System/Resources/Documentation") / (fsbuild.REPO + ".pdf")
    assert (system / flat).read_bytes().startswith(b"%PDF-")
    assert not (system / old.parent).exists()
    assert personal.read_bytes() == b"personal document"
    owner = subprocess.check_output(["dpkg-query", "--admindir", system / "var/lib/dpkg",
                                     "--search", "/" + str(flat)], text=True).strip()
    assert owner == fsbuild.PACKAGE + ": /" + str(flat), owner
print(fsbuild.PACKAGE + ": flat manual ownership and old folder removal passed")
