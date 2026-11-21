"""Check the exact regular-file payload of a built filesystem Debian package."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

import build
import release

parser = argparse.ArgumentParser()
parser.add_argument("package", type=Path)
parser.add_argument("--profile", choices=("portable", "aggressive"), required=True)
args = parser.parse_args()
fields = dict(line.split(": ", 1) for line in subprocess.check_output(
    ["dpkg-deb", "-f", str(args.package)], text=True).splitlines() if ": " in line and not line.startswith(" "))
assert fields["X-Infiltrator-Native-Profile"] == args.profile
assert fields["X-Infiltrator-Source-Revision"] == release.source_revision()
with tempfile.TemporaryDirectory(prefix="filesystem-package-check-") as temporary:
    root = Path(temporary)
    subprocess.run(["dpkg-deb", "-x", str(args.package), str(root)], check=True)
    module = root / "System/Filesystems" / build.NAME
    manual = root / "System/Resources/Documentation" / (build.REPO + ".pdf")
    expected = {module, manual}
    assert not any(p.is_dir() for p in manual.parent.iterdir())
    build.inspect(module)
    assert manual.read_bytes().startswith(b"%PDF-")
    assert fields["X-Infiltrator-Resource-SHA256"] == hashlib.sha256(module.read_bytes()).hexdigest()
    assert fields["X-Infiltrator-PDF-SHA256"] == hashlib.sha256(manual.read_bytes()).hexdigest()
    actual = {path for path in root.rglob("*") if path.is_symlink() or not path.is_dir()}
    if actual != expected or any(path.is_symlink() or not path.is_file() for path in expected):
        raise ValueError("Unexpected or missing filesystem package payload")
print("FILESYSTEM-PACKAGE-CONTENTS-PASS", build.REPO, args.profile)
