#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Portable DEB = standard SO+PDF. Source RUN = CPU-native SO+PDF Debian build.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
source "$root/packaging/fs-release.conf"
version="$(tr -d '\r\n' < "$root/VERSION")"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || exit 1
[[ "$FS_REPO" == Filesystem-* && "$FS_SO" == *.so && "$FS_PDF" == docs/*.pdf ]] || exit 1
slug="$(printf %s "$FS_REPO" | tr '[:upper:]' '[:lower:]')"
pkg="infiltrator-$slug"
module="/System/Filesystems/$FS_SO"
manual="/System/Resources/Documentation/$FS_REPO/$(basename "$FS_SO" .so).pdf"
action="$1"; shift
profile=aggressive; build_only=0; output=
while (($#)); do
 case "$1" in
  --profile) profile="$2"; shift 2 ;;
  --system-package-mode) shift ;;
  --build-only) build_only=1; shift ;;
  --output) output="$2"; shift 2 ;;
  *) echo "Unsupported option $1" >&2; exit 2 ;;
 esac
done
[[ "$profile" == portable || "$profile" == aggressive ]] || exit 2
if [[ -f "$root/SOURCE-REVISION" ]]; then
 revision="$(cat "$root/SOURCE-REVISION")"
else
 revision="$(git -C "$root" rev-parse HEAD)"
fi
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || exit 1
deps() {
 local missing=0 cmd p
 for cmd in cc c++ cmake make git readelf nm dpkg-deb dpkg-query sha256sum tar gzip awk pkg-config; do
  command -v "$cmd" >/dev/null || missing=1
 done
 for p in build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev; do
  dpkg -s "$p" 2>/dev/null | grep -Fxq "Status: install ok installed" || missing=1
 done
 if ((missing)); then
  if ((EUID)); then sudo apt-get -o DPkg::Lock::Timeout=-1 update; else apt-get -o DPkg::Lock::Timeout=-1 update; fi
  if ((EUID)); then
   sudo apt-get -o DPkg::Lock::Timeout=-1 install -y --no-install-recommends build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev
  else
   apt-get -o DPkg::Lock::Timeout=-1 install -y --no-install-recommends build-essential binutils cmake git pkg-config libssl-dev libsqlite3-dev dpkg-dev
  fi
 fi
}
inspect_so() {
 local f="$1"
 [[ -f "$f" && ! -L "$f" ]] || exit 1
 readelf -h "$f" | grep -Fq 'DYN (Shared object file)'
 nm -D "$f" > "$f.exports"
 grep -Fq "$FS_ABI" "$f.exports" || { echo "Missing ABI $FS_ABI" >&2; exit 1; }
 rm -f "$f.exports"
}
build_so() {
 local mode="$1" dir="$2" flags
 mkdir -p "$dir"
 if [[ "$FS_KIND" == make ]]; then
  if [[ "$mode" == portable ]]; then
   make -C "$root" OUT="$dir" CFLAGS="-std=c11 -O2 -march=x86-64 -mtune=generic -Wall -Wextra -Werror" test
  else
   mkdir -p "$dir/profile"
   make -C "$root" OUT="$dir" PROFILE_TRAIN=1 CFLAGS="-std=c11 -O3 -march=native -mtune=native -flto -fprofile-generate=$dir/profile -Wall -Wextra -Werror" test
   find "$dir/profile" -name '*.gcda' -type f -size +0c | grep -q .
   make -C "$root" OUT="$dir" clean
   make -C "$root" OUT="$dir" CFLAGS="-std=c11 -O3 -march=native -mtune=native -flto -fprofile-use=$dir/profile -fprofile-correction -Wno-missing-profile -Wall -Wextra -Werror" test
  fi
 elif [[ "$mode" == portable ]]; then
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_C_FLAGS_RELEASE="-O2 -march=x86-64 -mtune=generic -DNDEBUG" \
   -DCMAKE_CXX_FLAGS_RELEASE="-O2 -march=x86-64 -mtune=generic -DNDEBUG"
  cmake --build "$dir" --parallel 2
  ctest --test-dir "$dir" --output-on-failure
 else
  flags="-O3 -march=native -mtune=native -flto -DNDEBUG"
  mkdir -p "$dir/profile"
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_C_FLAGS_RELEASE="$flags -fprofile-generate=$dir/profile" \
   -DCMAKE_CXX_FLAGS_RELEASE="$flags -fprofile-generate=$dir/profile" \
   -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-generate=$dir/profile"
  cmake --build "$dir" --parallel 2
  ctest --test-dir "$dir" --output-on-failure
  find "$dir/profile" -name '*.gcda' -type f -size +0c | grep -q . || { echo 'No measured native PGO data' >&2; exit 1; }
  cmake -S "$root" -B "$dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_C_FLAGS_RELEASE="$flags -fprofile-use=$dir/profile -fprofile-correction -Wno-missing-profile" \
   -DCMAKE_CXX_FLAGS_RELEASE="$flags -fprofile-use=$dir/profile -fprofile-correction -Wno-missing-profile" \
   -DCMAKE_SHARED_LINKER_FLAGS="-fprofile-use=$dir/profile"
  cmake --build "$dir" --clean-first --parallel 2
  ctest --test-dir "$dir" --output-on-failure
 fi
 inspect_so "$dir/$FS_SO"
}
make_deb() {
 local mode="$1" scratch="$2" deb="$3" stage="$2/stage" rel="$version" flags="-O2 -march=x86-64 -mtune=generic" pgo=disabled lto=disabled
 build_so "$mode" "$scratch/build"
 if [[ "$mode" == aggressive ]]; then
  rel="$version+native1"; flags="-O3 -march=native -mtune=native -flto -fprofile-use"; pgo=trained-two-pass; lto=enabled
 fi
 install -Dm0755 "$scratch/build/$FS_SO" "$stage$module"
 install -Dm0644 "$root/$FS_PDF" "$stage$manual"
 [[ "$(head -c 5 "$stage$manual")" == '%PDF-' ]] || exit 1
 local sohash pdfhash depends="libc6" lib
 sohash="$(sha256sum "$stage$module" | cut -d' ' -f1)"
 pdfhash="$(sha256sum "$stage$manual" | cut -d' ' -f1)"
 while IFS= read -r lib; do
  case "$lib" in
   libc.so.6|libm.so.6|libpthread.so.0|ld-linux-x86-64.so.2) ;;
   libcrypto.so.3) depends="$depends, libssl3t64 | libssl3" ;;
   libsqlite3.so.0) depends="$depends, libsqlite3-0" ;;
   libstdc++.so.6) depends="$depends, libstdc++6" ;;
   libgcc_s.so.1) depends="$depends, libgcc-s1" ;;
   *) echo "Unqualified linked runtime dependency: $lib" >&2; exit 1 ;;
  esac
 done < <(readelf -d "$stage$module" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' | sort -u)
 mkdir -p "$stage/DEBIAN" "$(dirname "$deb")"
 cat > "$stage/DEBIAN/control" <<EOF
Package: $pkg
Version: $rel
Architecture: amd64
Section: admin
Priority: optional
Maintainer: Infiltrator Projects <noreply@infiltrator-projects.invalid>
Depends: $depends
X-Infiltrator-Source-Revision: $revision
X-Infiltrator-Native-Profile: $mode
X-Infiltrator-LTO: $lto
X-Infiltrator-PGO: $pgo
X-Infiltrator-Cflags: $flags
X-Infiltrator-Resource-SHA256: $sohash
X-Infiltrator-PDF-SHA256: $pdfhash
Description: Independent $FS_SO filesystem module and illustrated PDF
 Two-file payload only.
EOF
 dpkg-deb --build --root-owner-group "$stage" "$deb"
 mkdir -p "$scratch/check"
 dpkg-deb -x "$deb" "$scratch/check"
 local actual expected
 actual="$(cd "$scratch/check" && find . -type f -printf '%P\n' | LC_ALL=C sort)"
 expected="$(printf '%s\n' "$(echo "$module" | cut -c2-)" "$(echo "$manual" | cut -c2-)" | LC_ALL=C sort)"
 [[ "$actual" == "$expected" ]] || { echo "Expected exactly SO+PDF; got $actual" >&2; exit 1; }
 [[ "$(sha256sum "$scratch/check$module" | cut -d' ' -f1)" == "$sohash" ]] || exit 1
 [[ "$(sha256sum "$scratch/check$manual" | cut -d' ' -f1)" == "$pdfhash" ]] || exit 1
 echo "Qualified $mode $deb"
}
make_run() {
 local dest="$1" scratch="$2" hash
 git -C "$root" archive --format=tar HEAD > "$scratch/source.tar"
 printf '%s\n' "$revision" > "$scratch/SOURCE-REVISION"
 tar -rf "$scratch/source.tar" -C "$scratch" SOURCE-REVISION
 gzip -n < "$scratch/source.tar" > "$scratch/source.tar.gz"
 hash="$(sha256sum "$scratch/source.tar.gz" | cut -d' ' -f1)"
 cat > "$dest" <<'EOF'
#!/bin/sh
set -eu
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
line=$(awk '/^__IFS_SOURCE_PAYLOAD__$/{print NR+1;exit}' "$0")
test -n "$line"
tail -n +"$line" "$0" > "$work/source.tar.gz"
printf '%s  %s\n' 'SOURCE_HASH_PLACEHOLDER' "$work/source.tar.gz" | sha256sum -c - >/dev/null
mkdir "$work/source"
tar -xzf "$work/source.tar.gz" -C "$work/source" --no-same-owner --no-same-permissions
bash "$work/source/packaging/native-release.sh" install "$@"
exit $?
__IFS_SOURCE_PAYLOAD__
EOF
 sed -i "s/SOURCE_HASH_PLACEHOLDER/$hash/" "$dest"
 cat "$scratch/source.tar.gz" >> "$dest"
 chmod 0755 "$dest"
}
case "$action" in
 release)
  [[ -d "$root/.git" ]] || exit 1
  [[ ! -d "$root/dist" || -z "$(ls -A "$root/dist")" ]] || { echo 'Non-empty dist; refusing overwrite' >&2; exit 1; }
  deps
  temp="$(mktemp -d)"; trap 'rm -rf -- "$temp"' EXIT
  make_deb portable "$temp/portable" "$root/dist/$pkg"_"$version"_amd64.deb
  make_run "$root/dist/$slug-$version-native-installer.run" "$temp"
  ;;
 install)
  deps
  temp="$(mktemp -d)"; trap 'rm -rf -- "$temp"' EXIT
  if [[ -z "$output" ]]; then output="$temp/out"; fi
  if ((build_only)); then
   make_deb "$profile" "$temp/native" "$output/$pkg"_"$version"+native1_amd64.deb
  else
   deb="$temp/out/$pkg"_"$version"+native1_amd64.deb
   make_deb "$profile" "$temp/native" "$deb"
   if ((EUID)); then sudo dpkg -i "$deb"; else dpkg -i "$deb"; fi
   inspect_so "$module"
   [[ "$(sha256sum "$module" | cut -d' ' -f1)" == "$(dpkg-deb -f "$deb" X-Infiltrator-Resource-SHA256)" ]] || exit 1
   [[ "$(dpkg-query -S "$module")" == "$pkg: $module" ]] || exit 1
   [[ "$(dpkg-query -S "$manual")" == "$pkg: $manual" ]] || exit 1
  fi
  ;;
 *) exit 2 ;;
esac
