/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native disposable NTFS media seeding for qualification only.
 *
 * This test harness uses source-embedded NTFS helper functions linked into
 * this executable; it does not invoke ntfscp, ntfs-3g, FUSE, or an external
 * filesystem operation. Production writes are still exclusively Common V2.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/layout.h>

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: ntfs-native-seed IMAGE SOURCE DESTINATION\n");
        return 2;
    }
    setlocale(LC_ALL, "");
    const char *destination = argv[3];
    if (destination[0] == '/') ++destination;
    if (!*destination || strchr(destination, '/')) {
        fprintf(stderr, "Fixture destination must be one root filename\n");
        return 2;
    }
    int source = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) {
        perror("open fixture source");
        return 1;
    }
    ntfs_volume *volume = ntfs_mount(argv[1], NTFS_MNT_EXCLUSIVE);
    if (!volume) {
        perror("open writable disposable NTFS image");
        close(source);
        return 1;
    }
    int result = 1;
    ntfs_inode *root = NULL, *file = NULL;
    ntfs_attr *stream = NULL;
    ntfschar *unicode = NULL;
    root = ntfs_inode_open(volume, FILE_root);
    if (!root) {
        perror("open NTFS root");
        goto done;
    }
    const int chars = ntfs_mbstoucs(destination, &unicode);
    if (chars < 1 || chars > 255 || !unicode) {
        fprintf(stderr, "Cannot encode NTFS fixture filename\n");
        goto done;
    }
    file = ntfs_create(root, const_cpu_to_le32(0), unicode,
                       (u8)chars, S_IFREG);
    if (!file) {
        perror("create NTFS fixture file");
        goto done;
    }
    stream = ntfs_attr_open(file, AT_DATA, AT_UNNAMED, 0);
    if (!stream) {
        perror("open NTFS fixture data");
        goto done;
    }
    uint64_t offset = 0;
    for (;;) {
        uint8_t buffer[4096];
        ssize_t got = read(source, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            perror("read fixture input");
            goto done;
        }
        if (got == 0) break;
        if (offset > INT64_MAX - (uint64_t)got) {
            fprintf(stderr, "Fixture payload exceeds signed NTFS offset\n");
            goto done;
        }
        const s64 written = ntfs_attr_pwrite(stream, (s64)offset,
                                             (s64)got, buffer);
        if (written != (s64)got) {
            perror("write NTFS fixture payload");
            goto done;
        }
        offset += (uint64_t)got;
    }
    result = 0;
done:
    if (stream) ntfs_attr_close(stream);
    if (file && ntfs_inode_close(file)) result = 1;
    if (root && ntfs_inode_close(root)) result = 1;
    if (ntfs_umount(volume, FALSE)) result = 1;
    ntfs_ucsfree(unicode);
    if (close(source)) result = 1;
    return result;
}
