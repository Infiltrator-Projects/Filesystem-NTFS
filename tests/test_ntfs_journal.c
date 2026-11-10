// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_transaction.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    NtfsJournal state = {0};
    if (ntfs_journal_save(NULL, &state, NULL) == 0 ||
        ntfs_journal_save("", &state, NULL) == 0 ||
        ntfs_journal_save("/tmp/not-used", NULL, NULL) == 0 ||
        ntfs_journal_load(NULL, &state, NULL) == 0 ||
        ntfs_journal_load("", &state, NULL) == 0 ||
        ntfs_journal_load("/tmp/not-used", NULL, NULL) == 0 ||
        ntfs_journal_phase(NULL, &state, "ready", NULL) == 0 ||
        ntfs_journal_phase("/tmp/not-used", &state, "ready\ninvalid", NULL) == 0 ||
        ntfs_journal_phase("/tmp/not-used", &state,
                           "a-phase-name-far-too-long-for-journal", NULL) == 0) {
        fputs("NTFS journal accepted an invalid recovery state\n", stderr);
        return 1;
    }
    /* Verify that every recognised numeric key survives strict parsing.
     * A malformed or unrecognised key must still fail closed. */
    FILE *fixture = tmpfile();
    if (!fixture) return 1;
    const char *record =
        "DEFRAGMENTER-NTFS-JOURNAL-1\n"
        "device=/dev/example\n"
        "target_identity=test-identity\n"
        "serial=0123456789ABCDEF\n"
        "operation=defrag\n"
        "phase=prepared\n"
        "stage=/tmp/example.stage\n"
        "plan=/tmp/example.plan\n"
        "physical_bytes=1048576\n"
        "filesystem_bytes=1048576\n"
        "commit_cluster=12\n"
        "move_clusters=8\n"
        "workspace_start=16\n"
        "workspace_clusters=4\n";
    if (fputs(record, fixture) < 0 || fflush(fixture) != 0) {
        fclose(fixture);
        return 1;
    }
    char path[80];
    if (snprintf(path, sizeof(path), "/proc/self/fd/%d", fileno(fixture)) <= 0) {
        fclose(fixture);
        return 1;
    }
    NtfsJournal loaded = {0};
    if (ntfs_journal_load(path, &loaded, NULL) != 0 ||
        loaded.physical_bytes != 1048576 ||
        loaded.filesystem_bytes != 1048576 ||
        loaded.commit_cluster != 12 || loaded.move_clusters != 8 ||
        loaded.workspace_start != 16 || loaded.workspace_clusters != 4 ||
        strcmp(loaded.phase, "prepared") != 0) {
        ntfs_journal_free(&loaded);
        fclose(fixture);
        fputs("NTFS rejected a valid recovery journal\n", stderr);
        return 1;
    }
    ntfs_journal_free(&loaded);
    if (fputs("unexpected_key=42\n", fixture) < 0 || fflush(fixture) != 0 ||
        ntfs_journal_load(path, &loaded, NULL) == 0) {
        ntfs_journal_free(&loaded);
        fclose(fixture);
        fputs("NTFS accepted an unknown journal field\n", stderr);
        return 1;
    }
    fclose(fixture);
    return 0;
}
