"""Build a provider's existing Linux adapter from its recorded source."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "standalone/kernel.json"
SPEC = json.loads(CONFIG.read_text()) if CONFIG.is_file() else None
QUALIFIED_RELEASE = "6.12.107"


def prepare_source():
    if not SPEC or "revision" not in SPEC:
        return
    destination = ROOT / "kernel-source"
    revision = SPEC["revision"]
    if destination.exists():
        if (destination / "SOURCE-REVISION").read_text().strip() != revision:
            raise RuntimeError("Kernel source snapshot revision mismatch")
        return
    url = "https://codeload.github.com/Infiltrator-Projects/Filesystems/tar.gz/" + revision
    with tempfile.TemporaryDirectory(prefix=".kernel-source-", dir=ROOT) as temporary:
        snapshot = Path(temporary) / "source"
        snapshot.mkdir()
        with urllib.request.urlopen(url, timeout=120) as response, tarfile.open(fileobj=response, mode="r|gz") as archive:
            for member in archive:
                prefix, _, name = member.name.partition("/")
                if prefix != "Filesystems-" + revision or ".." in Path(name).parts:
                    raise RuntimeError("Invalid kernel source archive path")
                if not member.isfile() or not any(name == path or name.startswith(path + "/") for path in SPEC["paths"]):
                    continue
                path = snapshot / name
                path.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, path.open("wb") as output:
                    shutil.copyfileobj(source, output)
        for name in SPEC["paths"]:
            if not (snapshot / name).exists():
                raise RuntimeError("Missing recorded kernel source " + name)
        (snapshot / "SOURCE-REVISION").write_text(revision + "\n")
        snapshot.replace(destination)


def source_files():
    if not SPEC or "revision" not in SPEC:
        return []
    source = ROOT / "kernel-source"
    if (source / "SOURCE-REVISION").read_text().strip() != SPEC["revision"]:
        raise RuntimeError("Kernel source snapshot missing from native installer")
    return [path.relative_to(ROOT) for path in source.rglob("*") if path.is_file() and not path.is_symlink()]


def target(build_directory):
    running = os.uname().release
    directory = (build_directory or Path("/lib/modules") / running / "build").resolve()
    header = directory / "include/generated/utsrelease.h"
    symbols = directory / "Module.symvers"
    if not header.is_file() or not symbols.is_file() or not symbols.stat().st_size:
        raise RuntimeError("Prepared Linux headers with Module.symvers required at " + str(directory))
    release = re.fullmatch(r'#define UTS_RELEASE "([A-Za-z0-9.+_~-]+)"', header.read_text().strip())
    if not release or (build_directory is None and release[1] != running):
        raise RuntimeError("Linux build directory has the wrong kernel release")
    return directory, release[1]


def metadata(module, release):
    if module.is_symlink() or not module.is_file():
        raise RuntimeError("Kernel module must be a regular file")
    report = subprocess.check_output(["readelf", "-W", "-h", "-p", ".modinfo", str(module)],
                                     text=True, env={**os.environ, "LC_ALL": "C"})
    if (not re.search(r"^\s*Class:\s+ELF64\s*$", report, re.M)
            or not re.search(r"^\s*Type:\s+REL\b", report, re.M)
            or not re.search(r"^\s*Machine:.*X86-64\s*$", report, re.M)
            or not re.search(r"\bname=" + re.escape(SPEC["name"]) + r"\s*$", report, re.M)
            or not re.search(r"\bvermagic=" + re.escape(release) + r"(?=\s|$)", report)):
        raise RuntimeError("Kernel module identity or Linux ABI mismatch")
    return {"X-Infiltrator-Kernel-SHA256": hashlib.sha256(module.read_bytes()).hexdigest(),
            "X-Infiltrator-Kernel-Release": release}


def build(destination, selected_target):
    directory, release = selected_target
    source = ROOT / "kernel-source" if "revision" in SPEC else ROOT
    if "revision" in SPEC:
        source_files()
    with tempfile.TemporaryDirectory(prefix="filesystem-kernel-build-") as temporary:
        work = Path(temporary) / "source"
        paths = SPEC.get("paths", [SPEC["directory"]])
        for name in paths:
            original = source / name
            if original.is_dir():
                shutil.copytree(original, work / name)
            else:
                (work / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(original, work / name)
        module_directory = work / SPEC["directory"]
        if SPEC.get("makefile"):
            shutil.copyfile(module_directory / SPEC["makefile"], module_directory / "Makefile")
        if SPEC.get("prepare"):
            subprocess.run(SPEC["prepare"], cwd=module_directory, check=True)
        subprocess.run(["make", "-C", str(directory), "M=" + str(module_directory),
                        "-j" + os.environ.get("FILESYSTEM_KERNEL_JOBS", "2"),
                        *SPEC.get("make_args", []), "modules"], check=True)
        built = module_directory / (SPEC["name"] + ".ko")
        metadata(built, release)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(built, destination)
    return metadata(destination, release)
