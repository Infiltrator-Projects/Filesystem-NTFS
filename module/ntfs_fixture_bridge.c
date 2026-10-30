/* SPDX-License-Identifier: GPL-3.0-or-later
 * Only generic corpus operations are dispatched to the application's host.
 * All NTFS-specific geometry, block pointers, labels, payload placement,
 * allocation analysis and formatting belong exclusively to NTFS.so.
 */
#include "ifs_ntfs_fixture_v1.h"
#include <errno.h>
int ifs_ntfs_raw_fixture(const LdtmFilesystemSpec *,const char *,bool);
static _Thread_local const IfsNtfsFixtureHostV1 *active;
uint64_t ldtm_raw_file_bytes(const LdtmFilesystemSpec *s, unsigned n) {
    return active ? active->file_bytes(s,n) : 0u;
}
void ldtm_raw_name(unsigned n, char out[16]) {
    if (active) active->name(n,out);
    else if (out) out[0]=0;
}
int ldtm_raw_open(const char *p, bool w, uint64_t *bytes) {
    return active ? active->open(p,w,bytes) : -1;
}
int ldtm_raw_read(int fd, uint64_t at, void *data, size_t bytes) {
    return active ? active->read(fd,at,data,bytes) : -1;
}
int ldtm_raw_write(int fd, uint64_t at, const void *data, size_t bytes) {
    return active ? active->write(fd,at,data,bytes) : -1;
}
int ldtm_raw_plan(const LdtmFilesystemSpec *s,uint64_t bytes,uint32_t unit,uint64_t first,LdtmRawPlan *plan) {
    return active ? active->plan(s,bytes,unit,first,plan) : -1;
}
int ldtm_raw_write_payload(int fd, const LdtmRawPlan *plan) {
    return active ? active->write_payload(fd,plan) : -1;
}
void ldtm_raw_check_init(LdtmRawCheck *check, const LdtmFilesystemSpec *spec) {
    if (active) active->check_init(check,spec);
}
int ldtm_raw_check_file(LdtmRawCheck *c,unsigned f,uint64_t bytes) {
    return active ? active->check_file(c,f,bytes) : -1;
}
int ldtm_raw_check_data(LdtmRawCheck *c,int fd,unsigned f,uint64_t phys,uint64_t logical,uint64_t bytes) {
    return active ? active->check_data(c,fd,f,phys,logical,bytes) : -1;
}
int ldtm_raw_check_finish(const LdtmRawCheck *c) {
    return active ? active->check_finish(c) : -1;
}
int ifs_ntfs_fixture_run(const void *v,const char *p,int create,const void *h) {
    const IfsNtfsFixtureHostV1 *ops=h;
    if (!v || !p || !ops || active || ops->abi_version != IFS_NTFS_FIXTURE_V1 ||
        ops->structure_bytes < sizeof(*ops) ||
        !ops->file_bytes || !ops->name || !ops->open ||
        !ops->read || !ops->write || !ops->plan || !ops->write_payload ||
        !ops->check_init || !ops->check_file || !ops->check_data || !ops->check_finish) {
        errno=EINVAL; return -1;
    }
    const LdtmFilesystemSpec *spec=v;
    if (spec->creator != LDTM_CREATOR_NTFS || !spec->label) {
        errno=EINVAL; return -1;
    }
    active=ops;
    int result=ifs_ntfs_raw_fixture(spec,p,create!=0);
    active=NULL;
    return result;
}
