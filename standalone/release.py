#!/usr/bin/env python3
"""Release exactly one filesystem's DEB and source-embedded native RUN."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

import build as fsbuild

ROOT = Path(__file__).resolve().parents[1]
REPO = fsbuild.REPO
SLUG = fsbuild.SLUG
PACKAGE = fsbuild.PACKAGE
MODULE = fsbuild.NAME
PDF = fsbuild.PDF
VERSION = (ROOT / "VERSION").read_text().strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", VERSION):
    raise SystemExit("Invalid standalone VERSION")
DIST = ROOT / "dist"

def run(args):
    subprocess.run([str(a) for a in args], check=True)

def source_revision():
    record = ROOT / "SOURCE-REVISION"
    if record.exists():
        revision = record.read_text().strip()
    else:
        revision = subprocess.check_output(
            ["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise RuntimeError("Unqualified filesystem source revision")
    return revision


def build_deb(profile, dest):
    with tempfile.TemporaryDirectory(prefix=SLUG + "-package-") as tmp:
        root = Path(tmp) / "root"
        module = root / "System/Filesystems" / MODULE
        module.parent.mkdir(parents=True)
        fsbuild.install_deps()
        flags = fsbuild.build(profile, module)
        fsbuild.test(module)
        # The fixture test is an ephemeral QA executable, never a filesystem driver.
        # The fixture executable is removed by the standalone test harness.
        info_dir = root / "System/Resources/Documentation" / REPO
        info_dir.mkdir(parents=True)
        manual = info_dir / PDF.name
        shutil.copyfile(ROOT / PDF, manual)
        if manual.read_bytes()[:5] != b"%PDF-":
            raise RuntimeError("Missing valid illustrated PDF")
        digest = hashlib.sha256(module.read_bytes()).hexdigest()
        pdf_digest = hashlib.sha256(manual.read_bytes()).hexdigest()
        revision = source_revision()
        control = root / "DEBIAN"
        control.mkdir()
        deb_version = VERSION + ("+native1" if profile == "aggressive" else "")
        (control / "control").write_text(
            f"Package: {PACKAGE}\nVersion: {deb_version}\nArchitecture: amd64\n"
            "Section: admin\nPriority: optional\n"
            "Maintainer: Infiltrator Projects <noreply@infiltrator-projects.invalid>\n"
            f"Depends: {fsbuild.dependencies(module)}\n"
            f"X-Infiltrator-Source-Revision: {revision}\n"
            f"X-Infiltrator-Native-Profile: {profile}\n"
            f"X-Infiltrator-LTO: {'enabled' if profile == 'aggressive' else 'disabled'}\n"
            f"X-Infiltrator-PGO: {'trained-two-pass' if profile == 'aggressive' else 'disabled'}\n"
            f"X-Infiltrator-Cflags: {' '.join(flags)}\n"
            f"X-Infiltrator-Resource-SHA256: {digest}\n"
            f"X-Infiltrator-PDF-SHA256: {pdf_digest}\n"
            f"Description: Independent {MODULE} filesystem shared object for InfiltratorOS\n"
            f" Installs only {MODULE} and its illustrated PDF.\n")
        dest.parent.mkdir(parents=True, exist_ok=True)
        run(["dpkg-deb", "--root-owner-group", "--build", root, dest])
        content = subprocess.check_output(["dpkg-deb", "--contents", str(dest)], text=True)
        if f"./System/Filesystems/{MODULE}" not in content:
            raise RuntimeError("Missing selected module in package")
        with tempfile.TemporaryDirectory(prefix="filesystem-payload-check-") as check:
            run(["dpkg-deb", "-x", dest, check])
            actual = sorted(str(p.relative_to(check)) for p in Path(check).rglob("*") if p.is_file())
            expected = sorted([f"System/Filesystems/{MODULE}",
                               f"System/Resources/Documentation/{REPO}/FAT32.pdf"])
            if actual != expected:
                raise RuntimeError("DEB must own one .so and one PDF: " + str(actual))
    return dest

def source_files():
    if (ROOT / ".git").exists():
        names = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "-z"])
        paths = [Path(os.fsdecode(p)) for p in names.split(b"\0") if p]
    else:
        paths = [p.relative_to(ROOT) for p in ROOT.rglob("*") if p.is_file()]
    # Source-exact self-contained archives, same deterministic PAX builder
    # as FAT/OFS. Never include a build result, a symlink, or a local Git dir.
    chosen = []
    for p in paths:
        q = p.as_posix()
        if (q.startswith((".git/", "dist/", "build/", "__pycache__/"))
                or "/__pycache__/" in q or "/.git/" in q):
            continue
        if (ROOT / p).is_file() and not (ROOT / p).is_symlink():
            chosen.append(p)
    # EXT3 shares an implementation with EXT4, but its native installer
    # embeds the exact pinned source; installation must never clone a driver.
    for p in fsbuild.embedded_source_files():
        if p not in chosen:
            chosen.append(p)
    if len(chosen) < 8:
        raise RuntimeError("Insufficient source files for full independent build")
    return sorted(set(chosen))

def create_run(path):
    with tempfile.TemporaryFile() as temp:
        with tarfile.open(fileobj=temp, mode="w:gz", format=tarfile.PAX_FORMAT) as tar:
            for p in source_files():
                data = (ROOT / p).read_bytes()
                entry = tarfile.TarInfo(p.as_posix())
                entry.size, entry.mode, entry.uid, entry.gid, entry.mtime = len(data), 0o644, 0, 0, 0
                tar.addfile(entry, io.BytesIO(data))
            revision = (source_revision() + "\n").encode("ascii")
            stamp = tarfile.TarInfo("SOURCE-REVISION")
            stamp.size, stamp.mode, stamp.uid, stamp.gid, stamp.mtime = len(revision), 0o644, 0, 0, 0
            tar.addfile(stamp, io.BytesIO(revision))
        temp.seek(0)
        payload = temp.read()
    digest = hashlib.sha256(payload).hexdigest()
    header = """#!/bin/sh
set -eu
if [ "$#" -gt 0 ] && [ "$1" = "--help" ]; then
    echo "Usage: __NAME__ [--profile aggressive] [--system-package-mode] [--build-only --output DIRECTORY]"
    exit 0
fi
temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT HUP INT TERM
line=$(awk '/^__IFS_SOURCE_PAYLOAD_BELOW__$/ {print NR + 1; exit}' "$0")
test -n "$line"
tail -n +"$line" "$0" > "$temp/source.tar.gz"
printf '%s  %s\n' '__SHA256__' "$temp/source.tar.gz" | sha256sum -c - >/dev/null
mkdir "$temp/source"
tar -xzf "$temp/source.tar.gz" -C "$temp/source" --no-same-owner --no-same-permissions
python3 "$temp/source/standalone/release.py" install "$@"
exit $?
__IFS_SOURCE_PAYLOAD_BELOW__
""".replace("__NAME__", path.name).replace("__SHA256__", digest)
    path.write_bytes(header.encode("utf-8") + payload)
    path.chmod(0o755)

def release():
    DIST.mkdir(exist_ok=True)
    if list(DIST.iterdir()):
        raise RuntimeError("Refusing to mix release artifacts; clean dist first")
    fsbuild.prepare_release_source()
    deb = build_deb("portable", DIST / f"{PACKAGE}_{VERSION}_amd64.deb")
    runfile = DIST / f"{SLUG}-{VERSION}-native-installer.run"
    create_run(runfile)
    for artifact in (deb, runfile):
        print(artifact.name, hashlib.sha256(artifact.read_bytes()).hexdigest())
    if len(list(DIST.iterdir())) != 2:
        raise RuntimeError("Release must contain exactly the DEB and RUN")

def install(rest):
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", choices=("portable", "aggressive"), default="aggressive")
    parser.add_argument("--system-package-mode", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--output", type=Path)
    opt = parser.parse_args(rest)
    with tempfile.TemporaryDirectory(prefix=SLUG + "-install-") as tmp:
        if opt.build_only and not opt.output:
            raise RuntimeError("--build-only requires --output DIRECTORY")
        output = ((opt.output / f"{PACKAGE}_{VERSION}+native1_amd64.deb")
                  if opt.build_only else
                  Path(tmp) / f"{PACKAGE}_{VERSION}+native1_amd64.deb").resolve()
        build_deb(opt.profile, output)
        if opt.build_only:
            print(output)
            return
        args = ["dpkg", "-i", output]
        if os.geteuid() != 0:
            args.insert(0, "sudo")
        run(args)
        installed = Path("/System/Filesystems") / MODULE
        fsbuild.inspect(installed)
        manual = Path("/System/Resources/Documentation") / REPO / PDF.name
        if not manual.is_file() or manual.read_bytes()[:5] != b"%PDF-":
            raise RuntimeError("Installed filesystem PDF missing")
        control = subprocess.check_output(["dpkg-query", "-s", PACKAGE], text=True)
        metadata = dict(line.split(": ", 1) for line in control.splitlines() if ": " in line)
        expected = {
            "Version": VERSION + ("+native1" if opt.profile == "aggressive" else ""),
            "X-Infiltrator-Native-Profile": opt.profile,
            "X-Infiltrator-Source-Revision": source_revision(),
            "X-Infiltrator-Resource-SHA256": hashlib.sha256(installed.read_bytes()).hexdigest(),
            "X-Infiltrator-PDF-SHA256": hashlib.sha256(manual.read_bytes()).hexdigest(),
        }
        if any(metadata.get(k) != v for k, v in expected.items()):
            raise RuntimeError("Installed filesystem Debian metadata mismatch")
        for path in (installed, manual):
            owner = subprocess.check_output(["dpkg-query", "-S", str(path)], text=True)
            if not owner.startswith(PACKAGE + ":"):
                raise RuntimeError("Filesystem package does not own " + str(path))
        print("Installed and verified", installed)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("command", choices=("release", "install"))
    args, rest = p.parse_known_args()
    if args.command == "release":
        if rest:
            p.error("Unrecognised release options")
        release()
    else:
        install(rest)

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")
