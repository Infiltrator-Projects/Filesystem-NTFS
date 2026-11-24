#!/usr/bin/env python3
"""Release one portable DEB and a revision-pinned, source-first native RUN."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import build as fsbuild
import manual as manual_builder

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
        runtime = fsbuild.install_deps()
        flags = fsbuild.build(profile, module, runtime)
        fsbuild.test(module)
        if hasattr(fsbuild, "cleanup_test_artifacts"):
            fsbuild.cleanup_test_artifacts(module)
        # The fixture test is an ephemeral QA executable, never a filesystem driver.
        # The fixture executable is removed by the standalone test harness.
        info_dir = root / "System/Resources/Documentation"
        info_dir.mkdir(parents=True)
        manual = info_dir / (REPO + ".pdf")
        manual_builder.build(manual, ROOT / PDF.with_suffix(".pages"), VERSION)
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
            f" Installs {MODULE} and its illustrated PDF.\n")
        dest.parent.mkdir(parents=True, exist_ok=True)
        run(["dpkg-deb", "--root-owner-group", "--build", root, dest])
        content = subprocess.check_output(["dpkg-deb", "--contents", str(dest)], text=True)
        if f"./System/Filesystems/{MODULE}" not in content:
            raise RuntimeError("Missing selected module in package")
        with tempfile.TemporaryDirectory(prefix="filesystem-payload-check-") as check:
            run(["dpkg-deb", "-x", dest, check])
            actual = {p.relative_to(check) for p in Path(check).rglob("*") if p.is_symlink() or not p.is_dir()}
            expected = {Path(f"System/Filesystems/{MODULE}"), Path(f"System/Resources/Documentation/{REPO}.pdf")}
            if actual != expected:
                raise RuntimeError("Unexpected filesystem package payload: " + str(sorted(actual)))
    return dest



def create_run(path):
    # The release asset contains a shell bootstrap, not a source archive.
    # Fetch the exact Git commit, independently verify its object ID, and
    # let the checked-out builder acquire its separately pinned dependencies.
    revision = source_revision()
    header = """#!/bin/sh
set -eu
if [ "$#" -gt 0 ] && [ "$1" = "--help" ]; then
    echo "Usage: __NAME__ [--profile aggressive] [--build-only --output DIRECTORY]"
    exit 0
fi
command -v git >/dev/null 2>&1 || { echo "git is required to fetch the pinned source" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required by the existing native builder" >&2; exit 1; }
temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT HUP INT TERM
git init -q "$temp/source"
git -C "$temp/source" fetch --quiet --depth=1 https://github.com/Infiltrator-Projects/Filesystem-NTFS.git __REVISION__
git -C "$temp/source" checkout --quiet --detach FETCH_HEAD
test "$(git -C "$temp/source" rev-parse HEAD)" = '__REVISION__'
python3 "$temp/source/standalone/release.py" install "$@"
""".replace("__NAME__", path.name).replace("__REVISION__", revision)
    path.write_text(header, encoding="utf-8")
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
    if opt.build_only and not opt.output:
        parser.error("--build-only requires --output DIRECTORY")
    fsbuild.install_deps()
    fsbuild.prepare_release_source()
    version = VERSION + ("+native1" if opt.profile == "aggressive" else "")
    with tempfile.TemporaryDirectory(prefix=SLUG + "-install-") as tmp:
        output = ((opt.output if opt.build_only else Path(tmp)) / f"{PACKAGE}_{version}_amd64.deb").resolve()
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
        manual = Path("/System/Resources/Documentation") / (REPO + ".pdf")
        if not manual.is_file() or manual.read_bytes()[:5] != b"%PDF-":
            raise RuntimeError("Installed filesystem PDF missing")
        control = subprocess.check_output(["dpkg-query", "-s", PACKAGE], text=True)
        metadata = dict(line.split(": ", 1) for line in control.splitlines() if ": " in line)
        expected = {
            "Version": version,
            "X-Infiltrator-Native-Profile": opt.profile,
            "X-Infiltrator-Source-Revision": source_revision(),
            "X-Infiltrator-Resource-SHA256": hashlib.sha256(installed.read_bytes()).hexdigest(),
            "X-Infiltrator-PDF-SHA256": hashlib.sha256(manual.read_bytes()).hexdigest(),
        }
        paths = [installed, manual]
        if any(metadata.get(k) != v for k, v in expected.items()):
            raise RuntimeError("Installed filesystem Debian metadata mismatch")
        for path in paths:
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
    elif args.command == "install":
        install(rest)

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ERROR: {exc}")
