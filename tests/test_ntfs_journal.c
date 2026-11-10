// SPDX-License-Identifier: GPL-3.0-or-later
#include "ntfs_transaction.h"
#include <string.h>
#include <stdio.h>

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
    return 0;
}
