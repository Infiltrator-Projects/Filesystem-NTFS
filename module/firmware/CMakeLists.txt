# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(NtfsFirmware LANGUAGES C)
add_library(ntfs-boot MODULE descriptor.c ../../native/ntfs_disk.c runtime.c)
set_target_properties(ntfs-boot PROPERTIES PREFIX "" OUTPUT_NAME NTFS C_VISIBILITY_PRESET hidden)
target_compile_features(ntfs-boot PRIVATE c_std_11)
target_include_directories(ntfs-boot PRIVATE "${COMMON_INCLUDE_DIR}" ../../native)
target_compile_options(ntfs-boot PRIVATE -Wall -Wextra -Wpedantic -Werror
    -ffreestanding -fno-builtin -fno-stack-protector -mno-red-zone
    -fno-tree-loop-distribute-patterns -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(ntfs-boot PRIVATE -nostdlib -Wl,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
